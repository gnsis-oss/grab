#include "drivers/desktop/x11/config_watch.hpp"
#include "frontends/cli/watch_daemon.hpp"
#include "grab/result.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <limits>
#include <nlohmann/json.hpp>    // IWYU pragma: keep
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#include <utility>

extern "C"
{
#include <sys/pidfd.h>
}

namespace grab::cli
{
    namespace
    {

        using OrderedJson = nlohmann::ordered_json;
        using ReadResult  = decltype( ::read( -1, nullptr, std::size_t{} ) );
        using WriteResult = decltype( ::write( -1, nullptr, std::size_t{} ) );

        constexpr int              successExitCode        = 0;
        constexpr int              failureExitCode        = 1;
        constexpr int              posixFailure           = -1;
        constexpr int              noWaitResult           = 0;
        constexpr ReadResult       noBytesRead            = 0;
        constexpr WriteResult      noBytesWritten         = 0;
        constexpr unsigned int     noPidfdFlags           = 0U;
        constexpr mode_t           daemonLogMode          = 0600;
        constexpr mode_t           daemonLockMode         = 0600;
        constexpr std::size_t      startupReadEnd         = 0U;
        constexpr std::size_t      startupWriteEnd        = 1U;
        constexpr std::size_t      startupDescriptorCount = 2U;
        constexpr char             daemonReady            = 'R';
        constexpr char             daemonFailed           = 'F';
        constexpr std::string_view runtimeDirectoryName   = "XDG_RUNTIME_DIR";
        constexpr std::string_view stateDirectoryName     = "grab";
        constexpr std::string_view fallbackDirectory      = "/tmp";
        constexpr std::string_view pidFilename            = "watch.pid";
        constexpr std::string_view statusFilename         = "watch-status.json";
        constexpr std::string_view logFilename            = "watch.log";
        constexpr std::string_view temporarySuffix        = ".tmp";
        constexpr std::string_view lockSuffix             = ".lock";
        constexpr std::string_view configField            = "config";
        constexpr std::string_view capturedField          = "captured";
        constexpr std::string_view errorsField            = "errors";
        constexpr std::string_view skippedField           = "skipped";
        constexpr std::string_view pausedField            = "paused";
        constexpr std::string_view scriptFailedField      = "script_failed";
        constexpr std::string_view lastCaptureField       = "last_capture";
        constexpr std::string_view pidField               = "pid";
        constexpr std::string_view liveField              = "live";
        constexpr std::string_view configsField           = "configs";
        constexpr auto             stopTimeout            = std::chrono::seconds{ 10 };
        constexpr auto             readFailure = static_cast<ReadResult>( posixFailure );
        constexpr auto writeFailure = static_cast<WriteResult>( posixFailure );

        enum class ProcessWaitResult : std::uint8_t
        {
            Exited,
            TimedOut,
            Failed,
        };

        class OwnedFd
        {
            public:

                explicit OwnedFd( int fd ) noexcept :
                    fd_( fd )
                {
                }

                OwnedFd( const OwnedFd& ) = delete;
                OwnedFd&
                operator=( const OwnedFd& ) = delete;

                OwnedFd( OwnedFd&& other ) noexcept :
                    fd_( other.release() )
                {
                }

                OwnedFd&
                operator=( OwnedFd&& other ) noexcept
                {
                    if( this != &other )
                    {
                        reset();
                        fd_ = other.release();
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
                    return fd_;
                }

                [[nodiscard]]
                int
                release() noexcept
                {
                    return std::exchange( fd_, posixFailure );
                }

                void
                reset() noexcept
                {
                    if( fd_ != posixFailure )
                    {
                        static_cast<void>( ::close( fd_ ) );
                        fd_ = posixFailure;
                    }
                }

            private:

                int fd_{ posixFailure };
        };

        struct DaemonProcessState
        {
                OwnedFd lock{ posixFailure };
                OwnedFd startup_writer{ posixFailure };
        };

        [[nodiscard]]
        DaemonProcessState&
        daemon_process_state()
        {
            static DaemonProcessState state;
            return state;
        }

        [[nodiscard]]
        std::string
        posix_error( std::string_view operation,
                     int              error_number )
        {
            return std::string{ operation } +
                   ": " +
                   std::error_code{ error_number, std::generic_category() }.message();
        }

        [[nodiscard]]
        bool
        is_ascii_space( char value ) noexcept
        {
            constexpr char horizontalTab  = '\t';
            constexpr char lineFeed       = '\n';
            constexpr char verticalTab    = '\v';
            constexpr char formFeed       = '\f';
            constexpr char carriageReturn = '\r';
            constexpr char space          = ' ';
            return value ==
                   horizontalTab ||
                   value ==
                   lineFeed ||
                   value ==
                   verticalTab ||
                   value ==
                   formFeed ||
                   value ==
                   carriageReturn ||
                   value == space;
        }

        [[nodiscard]]
        std::optional<std::string>
        read_environment( std::string_view name )
        {
            constexpr char    assignment = '=';
            const std::string prefix     = std::string{ name } + assignment;
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

        [[nodiscard]]
        grab::Result<void>
        ensure_parent_directory( const std::filesystem::path& path )
        {
            const auto parent = path.parent_path();
            if( parent.empty() )
            {
                return {};
            }

            std::error_code error;
            static_cast<void>( std::filesystem::create_directories( parent, error ) );
            if( error )
            {
                return grab::fail(
                    ErrorCode::ProviderFailed,
                    "cannot create " + parent.string() + ": " + error.message()
                );
            }
            return {};
        }

        void
        remove_file( const std::filesystem::path& path ) noexcept
        {
            std::error_code error;
            static_cast<void>( std::filesystem::remove( path, error ) );
        }

        [[nodiscard]]
        std::filesystem::path
        temporary_path( const std::filesystem::path& path )
        {
            return std::filesystem::path{
                path.string() + std::string{ temporarySuffix }
            };
        }

        [[nodiscard]]
        std::filesystem::path
        lock_path( const DaemonPaths& paths )
        {
            return std::filesystem::path{
                paths.pid_file.string() + std::string{ lockSuffix }
            };
        }

        [[nodiscard]]
        grab::Result<void>
        write_file_atomically( const std::filesystem::path& path,
                               std::string_view             contents )
        {
            auto directory = ensure_parent_directory( path );
            if( !directory.has_value() )
            {
                return std::unexpected( std::move( directory.error() ) );
            }

            const auto    temporary = temporary_path( path );
            std::ofstream output{ temporary, std::ios::binary | std::ios::trunc };
            if( !output.is_open() )
            {
                remove_file( temporary );
                return grab::fail( ErrorCode::ProviderFailed,
                                   "cannot open temporary file: " + temporary.string() );
            }
            output.write( contents.data(),
                          static_cast<std::streamsize>( contents.size() ) );
            if( !output.good() )
            {
                output.close();
                remove_file( temporary );
                return grab::fail( ErrorCode::ProviderFailed,
                                   "cannot write temporary file: " +
                                       temporary.string() );
            }
            output.close();
            if( !output )
            {
                remove_file( temporary );
                return grab::fail( ErrorCode::ProviderFailed,
                                   "cannot close temporary file: " +
                                       temporary.string() );
            }

            std::error_code rename_error;
            std::filesystem::rename( temporary, path, rename_error );
            if( rename_error )
            {
                remove_file( temporary );
                return grab::fail(
                    ErrorCode::ProviderFailed,
                    "cannot replace " + path.string() + ": " + rename_error.message()
                );
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<pid_t>
        read_pid_file( const std::filesystem::path& path )
        {
            std::ifstream input{ path, std::ios::binary };
            if( !input.is_open() )
            {
                return grab::fail( ErrorCode::SessionNotFound,
                                   "watch pid file is missing: " + path.string() );
            }

            std::string contents{
                std::istreambuf_iterator<char>{ input },
                std::istreambuf_iterator<char>{}
            };
            while( !contents.empty() && is_ascii_space( contents.front() ) )
            {
                contents.erase( contents.begin() );
            }
            while( !contents.empty() && is_ascii_space( contents.back() ) )
            {
                contents.pop_back();
            }

            std::int64_t           parsed{};
            const std::string_view contents_view{ contents };
            const auto [end, error] =
                std::from_chars( contents_view.begin(), contents_view.end(), parsed );
            const auto maximum_pid =
                static_cast<std::int64_t>( std::numeric_limits<pid_t>::max() );
            if( error !=
                std::errc{} ||
                end !=
                contents_view.end() ||
                parsed <=
                0 ||
                parsed > maximum_pid )
            {
                return grab::fail( ErrorCode::ProtocolError,
                                   "watch pid file is malformed: " + path.string() );
            }
            return static_cast<pid_t>( parsed );
        }

        [[nodiscard]]
        bool
        process_is_live( pid_t pid )
        {
            const auto process_path =
                std::filesystem::path{ "/proc" } / std::to_string( pid );
            std::error_code error;
            const bool      exists = std::filesystem::exists( process_path, error );
            return exists && !error;
        }

        [[nodiscard]]
        grab::Result<void>
        write_pid_file( const std::filesystem::path& path,
                        pid_t                        pid )
        {
            return write_file_atomically( path, std::to_string( pid ) + "\n" );
        }

        [[nodiscard]]
        grab::Result<void>
        prepare_pid_file( const DaemonPaths& paths )
        {
            std::error_code exists_error;
            const bool      pid_file_exists =
                std::filesystem::exists( paths.pid_file, exists_error );
            if( exists_error )
            {
                return grab::fail( ErrorCode::ProviderFailed,
                                   "cannot inspect watch pid file: " +
                                       exists_error.message() );
            }
            if( pid_file_exists )
            {
                auto existing = read_pid_file( paths.pid_file );
                if( existing.has_value() && process_is_live( *existing ) )
                {
                    return grab::fail( ErrorCode::SessionExists,
                                       "watch daemon is already running with pid " +
                                           std::to_string( *existing ) );
                }
                remove_file( paths.pid_file );
            }

            remove_file( paths.status_file );
            remove_file( temporary_path( paths.pid_file ) );
            remove_file( temporary_path( paths.status_file ) );
            return {};
        }

        [[nodiscard]]
        grab::Result<OwnedFd>
        acquire_daemon_lock( const DaemonPaths& paths )
        {
            const auto lock_file        = lock_path( paths );
            const auto lock_file_string = lock_file.string();
            const int  descriptor = ::creat( lock_file_string.c_str(), daemonLockMode );
            if( descriptor == posixFailure )
            {
                return grab::fail( ErrorCode::ProviderFailed,
                                   posix_error( "creat " + lock_file_string, errno ) );
            }

            OwnedFd   lock{ descriptor };
            const int duplicate = ::dup( lock.get() );
            if( duplicate == posixFailure )
            {
                return grab::fail( ErrorCode::ProviderFailed,
                                   posix_error( "dup " + lock_file_string, errno ) );
            }
            OwnedFd close_on_exec_lock{ duplicate };
            if( ::dup3( lock.get(), close_on_exec_lock.get(), O_CLOEXEC ) ==
                posixFailure )
            {
                return grab::fail( ErrorCode::ProviderFailed,
                                   posix_error( "dup3 " + lock_file_string, errno ) );
            }
            lock.reset();

            if( ::flock( close_on_exec_lock.get(), LOCK_EX | LOCK_NB ) == posixFailure )
            {
                const int error_number = errno;
                if( error_number == EWOULDBLOCK )
                {
                    auto existing = read_pid_file( paths.pid_file );
                    if( existing.has_value() && process_is_live( *existing ) )
                    {
                        return grab::fail( ErrorCode::SessionExists,
                                           "watch daemon is already running with pid " +
                                               std::to_string( *existing ) );
                    }
                    return grab::fail( ErrorCode::SessionExists,
                                       "watch daemon start is already in progress" );
                }
                return grab::fail( ErrorCode::ProviderFailed,
                                   posix_error( "flock " + lock_file_string,
                                                error_number ) );
            }
            return std::move( close_on_exec_lock );
        }

        [[nodiscard]]
        bool
        write_startup_state( int  descriptor,
                             char state ) noexcept
        {
            while( true )
            {
                const auto written = ::write( descriptor, &state, sizeof( state ) );
                if( written == writeFailure )
                {
                    if( errno == EINTR )
                    {
                        continue;
                    }
                    return false;
                }
                return written > noBytesWritten;
            }
        }

        void
        report_daemon_ready() noexcept
        {
            auto& startup_writer = daemon_process_state().startup_writer;
            if( startup_writer.get() == posixFailure )
            {
                return;
            }
            static_cast<void>( write_startup_state( startup_writer.get(),
                                                    daemonReady ) );
            startup_writer.reset();
        }

        [[nodiscard]]
        bool
        read_startup_state( int   descriptor,
                            char& state ) noexcept
        {
            while( true )
            {
                const auto received = ::read( descriptor, &state, sizeof( state ) );
                if( received == readFailure )
                {
                    if( errno == EINTR )
                    {
                        continue;
                    }
                    return false;
                }
                return received > noBytesRead;
            }
        }

        [[noreturn]]
        void
        fail_daemon_child( int descriptor ) noexcept
        {
            static_cast<void>( write_startup_state( descriptor, daemonFailed ) );
            ::_exit( failureExitCode );
        }

        void
        reap_first_child( pid_t child ) noexcept
        {
            int status{};
            while( ::waitpid( child, &status, 0 ) == posixFailure && errno == EINTR )
            {
            }
        }

        [[nodiscard]]
        grab::Result<void>
        redirect_standard_streams( const std::filesystem::path& log_file )
        {
            const std::string log_path   = log_file.string();
            const int         descriptor = ::creat( log_path.c_str(), daemonLogMode );
            if( descriptor == posixFailure )
            {
                return grab::fail( ErrorCode::ProviderFailed,
                                   posix_error( "creat " + log_path, errno ) );
            }
            OwnedFd                       log_descriptor{ descriptor };

            constexpr std::array<int, 3U> standardDescriptors{
                STDIN_FILENO,
                STDOUT_FILENO,
                STDERR_FILENO,
            };
            for( const int standard_descriptor : standardDescriptors )
            {
                if( ::dup2( descriptor, standard_descriptor ) == posixFailure )
                {
                    const int error_number = errno;
                    return grab::fail( ErrorCode::ProviderFailed,
                                       posix_error( "dup2", error_number ) );
                }
            }
            if( descriptor <= STDERR_FILENO )
            {
                static_cast<void>( log_descriptor.release() );
            }
            return {};
        }

        void
        print_to( std::FILE*       stream,
                  std::string_view text )
        {
            static_cast<void>(
                std::fwrite( text.data(), sizeof( char ), text.size(), stream )
            );
        }

        void
        print_stale_pid( pid_t pid )
        {
            print_to(
                stdout,
                "watch is not running (stale pid " + std::to_string( pid ) + ")\n"
            );
        }

        void
        remove_daemon_state( const DaemonPaths& paths ) noexcept
        {
            remove_file( paths.pid_file );
            remove_file( paths.status_file );
            remove_file( temporary_path( paths.pid_file ) );
            remove_file( temporary_path( paths.status_file ) );
        }

        void
        remove_daemon_state_if_pid( const DaemonPaths& paths,
                                    pid_t              expected_pid )
        {
            auto cleanup_lock = acquire_daemon_lock( paths );
            if( !cleanup_lock.has_value() )
            {
                return;
            }
            auto current_pid = read_pid_file( paths.pid_file );
            if( current_pid.has_value() && *current_pid == expected_pid )
            {
                remove_daemon_state( paths );
            }
        }

        [[nodiscard]]
        grab::Result<OrderedJson>
        read_status_file( const std::filesystem::path& path )
        {
            std::ifstream input{ path, std::ios::binary };
            if( !input.is_open() )
            {
                return grab::fail( ErrorCode::SessionNotFound,
                                   "watch status file is missing: " + path.string() );
            }
            try
            {
                auto document = OrderedJson::parse( input );
                if( !document.is_object() )
                {
                    return grab::fail( ErrorCode::ProtocolError,
                                       "watch status root is not an object" );
                }
                return document;
            }
            catch( const nlohmann::json::exception& error )
            {
                return grab::fail( ErrorCode::ProtocolError,
                                   "cannot parse watch status: " +
                                       std::string{ error.what() } );
            }
        }

        [[nodiscard]]
        OrderedJson
        live_status_document( const DaemonPaths& paths,
                              pid_t              pid )
        {
            auto document = read_status_file( paths.status_file );
            if( !document.has_value() )
            {
                return OrderedJson{
                    {    pidField, static_cast<std::int64_t>( pid )},
                    {   liveField,                             true},
                    {configsField,             OrderedJson::array()},
                };
            }

            auto& root = *document;
            root.erase( pidField );
            root.emplace( pidField, static_cast<std::int64_t>( pid ) );
            root.erase( liveField );
            root.emplace( liveField, true );
            const auto configs = root.find( configsField );
            if( configs == root.end() || !configs->is_array() )
            {
                root.erase( configsField );
                root.emplace( configsField, OrderedJson::array() );
            }
            return root;
        }

        [[nodiscard]]
        std::string_view
        bool_text( bool value )
        {
            return value ? "true" : "false";
        }

        void
        print_text_status( const OrderedJson& document,
                           pid_t              pid )
        {
            print_to( stdout, "watch is running (pid " + std::to_string( pid ) + ")\n" );
            for( const auto& config : document.at( configsField ) )
            {
                const std::string config_path = config.value( configField, "" );
                const auto captured = config.value( capturedField, std::uint64_t{} );
                const auto errors   = config.value( errorsField, std::uint64_t{} );
                const auto skipped  = config.value( skippedField, std::uint64_t{} );
                const bool paused   = config.value( pausedField, false );
                const bool script_failed = config.value( scriptFailedField, false );
                const std::string last_capture  = config.value( lastCaptureField, "" );
                std::string       line          = config_path;
                line                           += " captured=";
                line                           += std::to_string( captured );
                line                           += " errors=";
                line                           += std::to_string( errors );
                line                           += " skipped=";
                line                           += std::to_string( skipped );
                line                           += " paused=";
                line                           += bool_text( paused );
                line                           += " script_failed=";
                line                           += bool_text( script_failed );
                line                           += " last_capture=";
                line                           += last_capture;
                line                           += '\n';
                print_to( stdout, line );
            }
        }

        [[nodiscard]]
        ProcessWaitResult
        wait_for_process_exit( int pidfd ) noexcept
        {
            const auto deadline = std::chrono::steady_clock::now() + stopTimeout;
            while( true )
            {
                const auto now = std::chrono::steady_clock::now();
                if( now >= deadline )
                {
                    return ProcessWaitResult::TimedOut;
                }
                const auto remaining =
                    std::chrono::ceil<std::chrono::milliseconds>( deadline - now );
                const auto remaining_count = remaining.count();
                const auto maximum_timeout = static_cast<decltype( remaining_count )>(
                    std::numeric_limits<int>::max()
                );
                const int timeout =
                    static_cast<int>( std::min( remaining_count, maximum_timeout ) );

                pollfd descriptor{
                    .fd      = pidfd,
                    .events  = POLLIN,
                    .revents = 0,
                };
                const int poll_result = ::poll( &descriptor, 1U, timeout );
                if( poll_result == posixFailure )
                {
                    if( errno == EINTR )
                    {
                        continue;
                    }
                    return ProcessWaitResult::Failed;
                }
                if( poll_result == noWaitResult )
                {
                    return ProcessWaitResult::TimedOut;
                }
                if( ( descriptor.revents & ( POLLIN | POLLHUP ) ) != 0 )
                {
                    return ProcessWaitResult::Exited;
                }
                if( ( descriptor.revents & ( POLLERR | POLLNVAL ) ) != 0 )
                {
                    return ProcessWaitResult::Failed;
                }
            }
        }

    }    // namespace

    DaemonPaths
    DaemonPaths::standard()
    {
        std::filesystem::path root{ fallbackDirectory };
        const auto runtime_directory = read_environment( runtimeDirectoryName );
        if( runtime_directory.has_value() && !runtime_directory->empty() )
        {
            const std::filesystem::path candidate{ *runtime_directory };
            if( candidate.is_absolute() )
            {
                root = candidate / stateDirectoryName;
            }
        }
        return DaemonPaths{
            .pid_file    = root / pidFilename,
            .status_file = root / statusFilename,
            .log_file    = root / logFilename,
        };
    }

    grab::Result<void>
    daemonize( const DaemonPaths& paths )
    {
        auto log_directory = ensure_parent_directory( paths.log_file );
        if( !log_directory.has_value() )
        {
            return std::unexpected( std::move( log_directory.error() ) );
        }
        auto pid_directory = ensure_parent_directory( paths.pid_file );
        if( !pid_directory.has_value() )
        {
            return std::unexpected( std::move( pid_directory.error() ) );
        }
        auto daemon_lock = acquire_daemon_lock( paths );
        if( !daemon_lock.has_value() )
        {
            return std::unexpected( std::move( daemon_lock.error() ) );
        }
        auto pid_preparation = prepare_pid_file( paths );
        if( !pid_preparation.has_value() )
        {
            return std::unexpected( std::move( pid_preparation.error() ) );
        }

        std::array<int, startupDescriptorCount> startup_pipe{};
        if( ::pipe2( startup_pipe.data(), O_CLOEXEC ) == posixFailure )
        {
            const int error_number = errno;
            remove_daemon_state( paths );
            return grab::fail( ErrorCode::ProviderFailed,
                               posix_error( "pipe2", error_number ) );
        }
        OwnedFd     startup_reader{ startup_pipe.at( startupReadEnd ) };
        OwnedFd     startup_writer{ startup_pipe.at( startupWriteEnd ) };

        const pid_t first_child = ::fork();
        if( first_child == static_cast<pid_t>( posixFailure ) )
        {
            const int error_number = errno;
            remove_daemon_state( paths );
            return grab::fail( ErrorCode::ProviderFailed,
                               posix_error( "fork", error_number ) );
        }
        if( first_child > 0 )
        {
            startup_writer.reset();
            char       startup_state = daemonFailed;
            const bool state_received =
                read_startup_state( startup_reader.get(), startup_state );
            reap_first_child( first_child );
            if( !state_received || startup_state != daemonReady )
            {
                remove_daemon_state( paths );
                ::_exit( failureExitCode );
            }
            ::_exit( successExitCode );
        }

        startup_reader.reset();
        if( ::setsid() == static_cast<pid_t>( posixFailure ) )
        {
            fail_daemon_child( startup_writer.get() );
        }

        const pid_t second_child = ::fork();
        if( second_child == static_cast<pid_t>( posixFailure ) )
        {
            fail_daemon_child( startup_writer.get() );
        }
        if( second_child > 0 )
        {
            ::_exit( successExitCode );
        }

        auto redirected = redirect_standard_streams( paths.log_file );
        if( !redirected.has_value() )
        {
            fail_daemon_child( startup_writer.get() );
        }
        auto pid_written = write_pid_file( paths.pid_file, ::getpid() );
        if( !pid_written.has_value() )
        {
            fail_daemon_child( startup_writer.get() );
        }
        auto& process_state          = daemon_process_state();
        process_state.lock           = std::move( *daemon_lock );
        process_state.startup_writer = std::move( startup_writer );
        return {};
    }

    grab::Result<void>
    write_status( const DaemonPaths&                                   paths,
                  std::span<const std::pair<std::string,
                                            grab::screen::WatchStats>> stats )
    {
        try
        {
            OrderedJson configs = OrderedJson::array();
            for( const auto& [config_path, snapshot] : stats )
            {
                configs.push_back( OrderedJson{
                    {      configField,            config_path},
                    {    capturedField,      snapshot.captured},
                    {      errorsField,        snapshot.errors},
                    {     skippedField,       snapshot.skipped},
                    {      pausedField,        snapshot.paused},
                    {scriptFailedField, snapshot.script_failed},
                    { lastCaptureField,  snapshot.last_capture},
                } );
            }
            const OrderedJson document{
                {    pidField, static_cast<std::int64_t>( ::getpid() )},
                {   liveField,                                    true},
                {configsField,                    std::move( configs )},
            };
            auto written =
                write_file_atomically( paths.status_file, document.dump( 2 ) + "\n" );
            if( written.has_value() )
            {
                report_daemon_ready();
            }
            return written;
        }
        catch( const nlohmann::json::exception& error )
        {
            return grab::fail( ErrorCode::InternalFault,
                               "cannot serialize watch status: " +
                                   std::string{ error.what() } );
        }
        catch( const std::exception& error )
        {
            return grab::fail( ErrorCode::InternalFault,
                               "cannot write watch status: " +
                                   std::string{ error.what() } );
        }
    }

    int
    run_watch_stop( const DaemonPaths& paths )
    {
        auto pid = read_pid_file( paths.pid_file );
        if( !pid.has_value() )
        {
            print_to( stdout, "watch is not running\n" );
            return failureExitCode;
        }
        if( !process_is_live( *pid ) )
        {
            print_stale_pid( *pid );
            remove_daemon_state_if_pid( paths, *pid );
            return failureExitCode;
        }

        const int raw_pidfd = ::pidfd_open( *pid, noPidfdFlags );
        if( raw_pidfd == posixFailure )
        {
            const int error_number = errno;
            if( error_number == ESRCH )
            {
                print_stale_pid( *pid );
                remove_daemon_state_if_pid( paths, *pid );
                return failureExitCode;
            }
            print_to( stderr,
                      "grab: cannot open watch process: " +
                          posix_error( "pidfd_open", error_number ) +
                          "\n" );
            return failureExitCode;
        }
        const OwnedFd pidfd{ raw_pidfd };

        if( ::pidfd_send_signal( pidfd.get(), SIGTERM, nullptr, noPidfdFlags ) ==
            posixFailure )
        {
            const int error_number = errno;
            if( error_number == ESRCH )
            {
                remove_daemon_state_if_pid( paths, *pid );
                return successExitCode;
            }
            print_to( stderr,
                      "grab: cannot stop watch process: " +
                          posix_error( "pidfd_send_signal", error_number ) +
                          "\n" );
            return failureExitCode;
        }

        switch( wait_for_process_exit( pidfd.get() ) )
        {
            case ProcessWaitResult::Exited :
                remove_daemon_state_if_pid( paths, *pid );
                print_to(
                    stdout,
                    "stopped watch daemon (pid " + std::to_string( *pid ) + ")\n"
                );
                return successExitCode;
            case ProcessWaitResult::TimedOut :
                print_to( stderr,
                          "grab: watch daemon did not stop within 10 seconds\n" );
                return failureExitCode;
            case ProcessWaitResult::Failed :
                print_to( stderr, "grab: failed while waiting for watch daemon\n" );
                return failureExitCode;
        }
        return failureExitCode;
    }

    int
    run_watch_status( const DaemonPaths& paths,
                      bool               as_json )
    {
        auto pid = read_pid_file( paths.pid_file );
        if( !pid.has_value() )
        {
            print_to( stdout, "watch is not running\n" );
            return failureExitCode;
        }
        if( !process_is_live( *pid ) )
        {
            print_stale_pid( *pid );
            remove_daemon_state_if_pid( paths, *pid );
            return failureExitCode;
        }

        try
        {
            const OrderedJson document = live_status_document( paths, *pid );
            if( as_json )
            {
                print_to( stdout, document.dump() + "\n" );
            }
            else
            {
                print_text_status( document, *pid );
            }
            return successExitCode;
        }
        catch( const nlohmann::json::exception& error )
        {
            print_to(
                stderr,
                "grab: cannot render watch status: " + std::string{ error.what() } + "\n"
            );
            return failureExitCode;
        }
        catch( const std::exception& error )
        {
            print_to(
                stderr,
                "grab: cannot read watch status: " + std::string{ error.what() } + "\n"
            );
            return failureExitCode;
        }
    }

}    // namespace grab::cli
