#include "grab/result.hpp"
#include "kernel/support/posix_mkstemp.hpp"
#include "kernel/support/posix_open.h"
#include "spi/permission.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <system_error>
#include <unistd.h>

namespace grab::core
{

    PermissionState
    NoPermissionBroker::query( std::string_view /*permission*/ )
    {
        return PermissionState::Granted;
    }

    grab::Result<void>
    NoPermissionBroker::request( std::string_view /*permission*/ )
    {
        return {};
    }

    std::filesystem::path
    StateDir::resolve( const GetEnv& get_env )
    {
        if( const auto xdg = get_env( "XDG_STATE_HOME" );
            xdg.has_value() && !xdg->empty() )
        {
            return std::filesystem::path( *xdg ) / "grab";
        }
        const auto home = get_env( "HOME" ).value_or( "/" );
        return std::filesystem::path( home ) / ".local" / "state" / "grab";
    }

    grab::Result<void>
    StateDir::write_atomic( const std::filesystem::path& file,
                            std::string_view             contents )
    {
        try
        {
            constexpr int     posixFailure = -1;
            constexpr ssize_t writeFailure = -1;

            const auto        posix_error = []( std::string_view step,
                                                int              error_number ) -> std::string
            {
                return std::string{ step } +
                       ": " +
                       std::error_code{ error_number, std::generic_category() }
                           .message();
            };

            std::error_code ec;
            const auto      parent = file.parent_path();
            if( !parent.empty() )
            {
                std::filesystem::create_directories( parent, ec );
                if( ec )
                {
                    return grab::fail( grab::ErrorCode::InternalFault,
                                       "create_directories: " + ec.message() );
                }
            }

            // Restore tokens must be durable and safe against concurrent writers:
            // unique temp (mkstemp) + fsync-before-rename (legacy screengrab pattern).
            std::string temp = file.string() + ".tmp.XXXXXX";
            int         fd   = posix::mkstemp( temp.data() );
            if( fd == posixFailure )
            {
                return grab::fail( grab::ErrorCode::InternalFault,
                                   posix_error( "mkstemp", errno ) );
            }

            const auto cleanup_temp = [&]( std::string message ) -> std::string
            {
                if( fd != posixFailure )
                {
                    if( ::close( fd ) == posixFailure )
                    {
                        message += "; " + posix_error( "close", errno );
                    }
                    fd = posixFailure;
                }
                if( ::unlink( temp.c_str() ) == posixFailure )
                {
                    message += "; " + posix_error( "unlink", errno );
                }
                return message;
            };

            std::string_view unwritten = contents;
            while( !unwritten.empty() )
            {
                const auto count = std::min(
                    unwritten.size(),
                    static_cast<std::size_t>( std::numeric_limits<ssize_t>::max() )
                );
                const auto written = ::write( fd, unwritten.data(), count );
                if( written == writeFailure )
                {
                    const int error_number = errno;
                    if( error_number == EINTR )
                    {
                        continue;
                    }
                    return grab::fail( grab::ErrorCode::InternalFault,
                                       cleanup_temp( posix_error( "write",
                                                                  error_number ) ) );
                }
                if( written == 0 )
                {
                    return grab::fail( grab::ErrorCode::InternalFault,
                                       cleanup_temp( "write: wrote zero bytes" ) );
                }
                unwritten.remove_prefix( static_cast<std::size_t>( written ) );
            }

            if( ::fsync( fd ) == posixFailure )
            {
                return grab::fail( grab::ErrorCode::InternalFault,
                                   cleanup_temp( posix_error( "fsync", errno ) ) );
            }
            if( ::close( fd ) == posixFailure )
            {
                fd = posixFailure;
                return grab::fail( grab::ErrorCode::InternalFault,
                                   cleanup_temp( posix_error( "close", errno ) ) );
            }
            fd = posixFailure;

            std::filesystem::rename( temp, file, ec );
            if( ec )
            {
                return grab::fail( grab::ErrorCode::InternalFault,
                                   cleanup_temp( "rename: " + ec.message() ) );
            }
            const std::string sync_parent = parent.empty() ? "." : parent.string();
            if( grab_fsync_dir( sync_parent.c_str() ) == 0 )
            {
                return grab::fail( grab::ErrorCode::InternalFault,
                                   posix_error( "fsync dir", errno ) );
            }
            return {};
        }
        catch( ... )
        {
            return grab::fail( grab::ErrorCode::InternalFault,
                               "write_atomic: unexpected exception" );
        }
    }

}    // namespace grab::core
