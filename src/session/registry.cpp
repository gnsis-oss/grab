#include "core/posix_mkstemp.hpp"
#include "grab/result.hpp"
#include "session/record.hpp"
#include "session/registry.hpp"

// NOLINTBEGIN(llvm-include-order)
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <exception>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>
// NOLINTEND(llvm-include-order)

namespace grab::session
{
    namespace
    {

        constexpr int         posix_failure   = -1;
        constexpr mode_t      record_mode     = S_IRUSR | S_IWUSR;
        constexpr std::size_t read_chunk_size = 4'096U;
        constexpr char        env_assign      = '=';

        [[nodiscard]]
        std::optional<std::string>
        read_live_environment( std::string_view name )
        {
            const std::string prefix = std::string{ name } + env_assign;
            for( char* const* entry = ::environ; entry != nullptr && *entry != nullptr;
                 entry              = std::next( entry ) )
            {
                const std::string_view variable{ *entry };
                if( variable.starts_with( prefix ) )
                {
                    return std::string{ variable.substr( prefix.size() ) };
                }
            }

            return std::nullopt;
        }

        using ReadResult  = decltype( ::read( posix_failure, nullptr, std::size_t{} ) );
        using WriteResult = decltype( ::write( posix_failure, nullptr, std::size_t{} ) );

        constexpr auto read_failure  = static_cast<ReadResult>( posix_failure );
        constexpr auto write_failure = static_cast<WriteResult>( posix_failure );

        class OwnedFd
        {
            public:

                explicit OwnedFd( int fd ) noexcept :
                    fd( fd )
                {
                }

                OwnedFd( const OwnedFd& ) = delete;
                OwnedFd&
                operator=( const OwnedFd& ) = delete;

                OwnedFd( OwnedFd&& other ) noexcept :
                    fd( other.release() )
                {
                }

                OwnedFd&
                operator=( OwnedFd&& other ) noexcept
                {
                    if( this != &other )
                    {
                        reset();
                        fd = other.release();
                    }
                    return *this;
                }

                ~OwnedFd()
                {
                    reset();
                }

                [[nodiscard]]
                int
                get() const noexcept
                {
                    return fd;
                }

                [[nodiscard]]
                int
                release() noexcept
                {
                    return std::exchange( fd, posix_failure );
                }

                void
                reset() noexcept
                {
                    if( fd != posix_failure )
                    {
                        static_cast<void>( ::close( fd ) );
                        fd = posix_failure;
                    }
                }

            private:

                int fd = posix_failure;
        };

        [[nodiscard]]
        std::string
        posix_error( std::string_view step,
                     int              error_number )
        {
            return std::string{ step } +
                   ": " +
                   std::error_code{ error_number, std::generic_category() }.message();
        }

        [[nodiscard]]
        grab::Result<void>
        write_all( int              fd,
                   std::string_view contents )
        {
            std::string_view remaining = contents;
            while( !remaining.empty() )
            {
                const auto count = std::min(
                    remaining.size(),
                    static_cast<std::size_t>( std::numeric_limits<WriteResult>::max() )
                );
                const auto written = ::write( fd, remaining.data(), count );
                if( written == write_failure )
                {
                    const int error_number = errno;
                    if( error_number == EINTR )
                    {
                        continue;
                    }
                    return grab::fail( ErrorCode::internal_fault,
                                       posix_error( "write", error_number ) );
                }
                if( written == 0 )
                {
                    return grab::fail( ErrorCode::internal_fault,
                                       "write: wrote zero bytes" );
                }
                remaining.remove_prefix( static_cast<std::size_t>( written ) );
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<std::string>
        slurp_file( const std::filesystem::path& file )
        {
            const std::string file_text = file.string();
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
            const int         fd = ::open( file_text.c_str(), O_RDONLY | O_CLOEXEC );
            if( fd == posix_failure )
            {
                const int error_number = errno;
                if( error_number == ENOENT )
                {
                    return grab::fail( ErrorCode::session_not_found,
                                       "session record not found: " + file_text );
                }
                return grab::fail( ErrorCode::internal_fault,
                                   posix_error( "open", error_number ) );
            }

            const OwnedFd                     owned{ fd };
            std::string                       contents;
            std::array<char, read_chunk_size> buffer{};
            for( ;; )
            {
                const auto bytes_read =
                    ::read( owned.get(), buffer.data(), buffer.size() );
                if( bytes_read == read_failure )
                {
                    const int error_number = errno;
                    if( error_number == EINTR )
                    {
                        continue;
                    }
                    return grab::fail( ErrorCode::internal_fault,
                                       posix_error( "read", error_number ) );
                }
                if( bytes_read == 0 )
                {
                    break;
                }
                contents.append( buffer.data(), static_cast<std::size_t>( bytes_read ) );
            }
            return contents;
        }

        [[nodiscard]]
        grab::Result<void>
        fail_unlinking_temp( const std::string& temp,
                             grab::Error        error )
        {
            static_cast<void>( ::unlink( temp.c_str() ) );
            return std::unexpected( std::move( error ) );
        }

        [[nodiscard]]
        bool
        lock_is_held_error( int error_number ) noexcept
        {
            return error_number == EWOULDBLOCK;
        }

    }    // namespace

    SessionRegistry::SessionRegistry( std::filesystem::path root ) :
        registry_root( std::move( root ) )
    {
        std::error_code ec;
        std::filesystem::create_directories( registry_root, ec );
    }

    grab::Result<void>
    SessionRegistry::create( const SessionRecord& record )
    {
        try
        {
            const auto        path      = json_path( record.name );
            const std::string path_text = path.string();
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
            const int         fd = ::open( path_text.c_str(),
                                           O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                                           record_mode );
            if( fd == posix_failure )
            {
                const int error_number = errno;
                if( error_number == EEXIST )
                {
                    return grab::fail( ErrorCode::session_exists,
                                       "session already exists: " + record.name );
                }
                return grab::fail( ErrorCode::internal_fault,
                                   posix_error( "open", error_number ) );
            }

            const OwnedFd owned{ fd };
            const auto    json   = to_json( record );
            const auto    result = write_all( owned.get(), json );
            if( !result.has_value() )
            {
                return fail_unlinking_temp( path_text, result.error() );
            }
            return {};
        }
        catch( const std::exception& error )
        {
            return grab::fail( ErrorCode::internal_fault,
                               std::string{ "create: " } + error.what() );
        }
    }

    grab::Result<void>
    SessionRegistry::write( const SessionRecord& record )
    {
        try
        {
            const auto        final_path      = json_path( record.name );
            const std::string final_path_text = final_path.string();
            const auto        temp_path =
                registry_root / ( std::string{ "." } + record.name + ".XXXXXX" );
            std::string temp_path_text = temp_path.string();

            const int   fd             = core::posix::mkstemp( temp_path_text.data() );
            if( fd == posix_failure )
            {
                return grab::fail( ErrorCode::internal_fault,
                                   posix_error( "mkstemp", errno ) );
            }

            OwnedFd    owned{ fd };
            const auto json   = to_json( record );
            const auto result = write_all( owned.get(), json );
            if( !result.has_value() )
            {
                return fail_unlinking_temp( temp_path_text, result.error() );
            }

            owned.reset();
            if( ::rename( temp_path_text.c_str(), final_path_text.c_str() ) ==
                posix_failure )
            {
                const int error_number = errno;
                static_cast<void>( ::unlink( temp_path_text.c_str() ) );
                return grab::fail( ErrorCode::internal_fault,
                                   posix_error( "rename", error_number ) );
            }
            return {};
        }
        catch( const std::exception& error )
        {
            return grab::fail( ErrorCode::internal_fault,
                               std::string{ "write: " } + error.what() );
        }
    }

    grab::Result<SessionRecord>
    SessionRegistry::read( std::string_view name )
    {
        try
        {
            const auto contents = slurp_file( json_path( name ) );
            if( !contents.has_value() )
            {
                return std::unexpected( contents.error() );
            }
            return parse_record( *contents );
        }
        catch( const std::exception& error )
        {
            return grab::fail( ErrorCode::internal_fault,
                               std::string{ "read: " } + error.what() );
        }
    }

    grab::Result<int>
    SessionRegistry::acquire_liveness_lock( std::string_view name )
    {
        try
        {
            const auto        path      = lock_path( name );
            const std::string path_text = path.string();
            const int fd = ::open(    // NOLINT(cppcoreguidelines-pro-type-vararg)
                path_text.c_str(),
                O_RDWR | O_CREAT | O_CLOEXEC,
                record_mode
            );
            if( fd == posix_failure )
            {
                return grab::fail( ErrorCode::internal_fault,
                                   posix_error( "open", errno ) );
            }

            OwnedFd owned{ fd };
            if( ::flock( owned.get(), LOCK_EX | LOCK_NB ) == posix_failure )
            {
                const int error_number = errno;
                if( lock_is_held_error( error_number ) )
                {
                    return grab::fail( ErrorCode::session_exists,
                                       "session is already live: " +
                                           std::string{ name } );
                }
                return grab::fail( ErrorCode::internal_fault,
                                   posix_error( "flock", error_number ) );
            }

            return owned.release();
        }
        catch( const std::exception& error )
        {
            return grab::fail( ErrorCode::internal_fault,
                               std::string{ "acquire_liveness_lock: " } + error.what() );
        }
    }

    bool
    SessionRegistry::is_live( std::string_view name ) const
    {
        try
        {
            const auto        path      = lock_path( name );
            const std::string path_text = path.string();
            const int fd = ::open(    // NOLINT(cppcoreguidelines-pro-type-vararg)
                path_text.c_str(),
                O_RDWR | O_CREAT | O_CLOEXEC,
                record_mode
            );
            if( fd == posix_failure )
            {
                return false;
            }

            const OwnedFd owned{ fd };
            if( ::flock( owned.get(), LOCK_EX | LOCK_NB ) == posix_failure )
            {
                return lock_is_held_error( errno );
            }

            static_cast<void>( ::flock( owned.get(), LOCK_UN ) );
            return false;
        }
        catch( const std::exception& )
        {
            return false;
        }
    }

    std::vector<SessionRecord>
    SessionRegistry::list()
    {
        try
        {
            std::vector<SessionRecord> records;
            std::error_code            ec;
            for( std::filesystem::directory_iterator it{ registry_root, ec };
                 !ec && it != std::filesystem::directory_iterator{};
                 it.increment( ec ) )
            {
                const auto& entry = *it;
                if( !entry.is_regular_file( ec ) )
                {
                    ec.clear();
                    continue;
                }
                if( entry.path().extension() != ".json" )
                {
                    continue;
                }

                const auto contents = slurp_file( entry.path() );
                if( !contents.has_value() )
                {
                    continue;
                }
                auto record = parse_record( *contents );
                if( record.has_value() )
                {
                    records.push_back( std::move( *record ) );
                }
            }
            return records;
        }
        catch( const std::exception& )
        {
            return {};
        }
    }

    grab::Result<void>
    SessionRegistry::remove( std::string_view name )
    {
        try
        {
            const auto        record_path      = json_path( name );
            const std::string record_path_text = record_path.string();
            if( ::unlink( record_path_text.c_str() ) == posix_failure )
            {
                const int error_number = errno;
                if( error_number == ENOENT )
                {
                    return grab::fail( ErrorCode::session_not_found,
                                       "session record not found: " +
                                           std::string{ name } );
                }
                return grab::fail( ErrorCode::internal_fault,
                                   posix_error( "unlink", error_number ) );
            }

            const auto        live_path      = lock_path( name );
            const std::string live_path_text = live_path.string();
            if( ::unlink( live_path_text.c_str() ) == posix_failure )
            {
                const int error_number = errno;
                if( error_number != ENOENT )
                {
                    return grab::fail( ErrorCode::internal_fault,
                                       posix_error( "unlink", error_number ) );
                }
            }
            return {};
        }
        catch( const std::exception& error )
        {
            return grab::fail( ErrorCode::internal_fault,
                               std::string{ "remove: " } + error.what() );
        }
    }

    std::size_t
    SessionRegistry::reap_dead()
    {
        std::size_t count = 0U;
        for( const auto& record : list() )
        {
            if( is_live( record.name ) )
            {
                continue;
            }
            const auto removed = remove( record.name );
            if( removed.has_value() )
            {
                ++count;
            }
        }
        return count;
    }

    grab::Result<std::filesystem::path>
    SessionRegistry::default_root()
    {
        try
        {
            const std::optional<std::string> runtime_dir =
                read_live_environment( "XDG_RUNTIME_DIR" );
            if( !runtime_dir.has_value() || runtime_dir->empty() )
            {
                return grab::fail( ErrorCode::environment_changed,
                                   "XDG_RUNTIME_DIR is unset" );
            }
            return std::filesystem::path{ *runtime_dir } / "grab" / "sessions";
        }
        catch( const std::exception& error )
        {
            return grab::fail( ErrorCode::internal_fault,
                               std::string{ "default_root: " } + error.what() );
        }
    }

    std::filesystem::path
    SessionRegistry::json_path( std::string_view name ) const
    {
        return registry_root / ( std::string{ name } + ".json" );
    }

    std::filesystem::path
    SessionRegistry::lock_path( std::string_view name ) const
    {
        return registry_root / ( std::string{ name } + ".lock" );
    }

}    // namespace grab::session
