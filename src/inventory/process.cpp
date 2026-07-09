#include "grab/result.hpp"
#include "inventory/environment_merge.hpp"
#include "inventory/process.hpp"

// NOLINTBEGIN(llvm-include-order)
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <iterator>
#include <signal.h>    // NOLINT(modernize-deprecated-headers)
#include <span>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>
// NOLINTEND(llvm-include-order)

namespace grab::inventory
{

    namespace
    {

        constexpr int              posix_failure       = -1;
        constexpr pid_t            child_still_running = 0;
        constexpr std::string_view null_device_path    = "/dev/null";
        constexpr auto             terminate_grace     = std::chrono::seconds{ 5 };
        constexpr auto terminate_poll_interval = std::chrono::milliseconds{ 100 };

        [[nodiscard]]
        std::string
        posix_error_message( std::string_view operation,
                             int              error_number )
        {
            std::string message{ operation };
            message += ": ";
            message +=
                std::error_code{ error_number, std::generic_category() }.message();
            return message;
        }

        [[nodiscard]]
        std::vector<std::string>
        make_argv_storage( std::string_view             program,
                           std::span<const std::string> args )
        {
            std::vector<std::string> storage;
            storage.reserve( args.size() + 1U );
            storage.emplace_back( program );
            for( const std::string& arg : args )
            {
                storage.push_back( arg );
            }
            return storage;
        }

        [[nodiscard]]
        std::vector<char*>
        make_string_pointers( std::vector<std::string>& storage )
        {
            std::vector<char*> pointers;
            pointers.reserve( storage.size() + 1U );
            for( std::string& value : storage )
            {
                pointers.push_back( value.data() );
            }
            pointers.push_back( nullptr );
            return pointers;
        }

        [[nodiscard]]
        grab::Result<void>
        add_dup2_action( posix_spawn_file_actions_t& actions,
                         int                         source,
                         int                         destination )
        {
            const int status =
                posix_spawn_file_actions_adddup2( &actions, source, destination );
            if( status != 0 )
            {
                return grab::fail( grab::ErrorCode::provider_failed,
                                   posix_error_message( "posix_spawn adddup2",
                                                        status ) );
            }
            return {};
        }

        [[nodiscard]]
        bool
        wait_for_exit( pid_t                               pid,
                       std::chrono::steady_clock::duration timeout )
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while( std::chrono::steady_clock::now() < deadline )
            {
                int         status = 0;
                // NOLINTNEXTLINE(misc-include-cleaner)
                const pid_t result = ::waitpid( pid, &status, WNOHANG );
                if( result == pid )
                {
                    return true;
                }
                if( result == posix_failure )
                {
                    if( errno == EINTR )
                    {
                        continue;
                    }
                    return errno == ECHILD;
                }
                if( result != child_still_running )
                {
                    return true;
                }
                std::this_thread::sleep_for( terminate_poll_interval );
            }
            return false;
        }

    }    // namespace

    grab::Result<pid_t>
    launch_app( std::string_view                           apprun,
                std::span<const std::string>               args,
                const std::vector<std::pair<std::string,
                                            std::string>>& environment )
    {
        std::vector<std::string>      argv_storage = make_argv_storage( apprun, args );
        std::vector<char*>            argv = make_string_pointers( argv_storage );
        std::vector<std::string_view> base;
        for( char* const* entry = ::environ; entry != nullptr && *entry != nullptr;
             entry              = std::next( entry ) )
        {
            base.emplace_back( *entry );
        }
        std::vector<std::string> merged = merge_environment( base, environment );
        std::vector<char*>       envp   = make_string_pointers( merged );

        const std::string        null_device{ null_device_path };
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        const int null_fd = ::open( null_device.c_str(), O_WRONLY | O_CLOEXEC );
        if( null_fd == posix_failure )
        {
            return grab::fail( grab::ErrorCode::provider_failed,
                               posix_error_message( "open /dev/null", errno ) );
        }

        posix_spawn_file_actions_t actions{};
        int                        status = posix_spawn_file_actions_init( &actions );
        if( status != 0 )
        {
            ( void )::close( null_fd );
            return grab::fail( grab::ErrorCode::provider_failed,
                               posix_error_message( "posix_spawn actions init",
                                                    status ) );
        }

        auto stdout_action = add_dup2_action( actions, null_fd, STDOUT_FILENO );
        if( !stdout_action.has_value() )
        {
            ( void )posix_spawn_file_actions_destroy( &actions );
            ( void )::close( null_fd );
            return grab::fail( stdout_action.error().code,
                               stdout_action.error().message );
        }

        auto stderr_action = add_dup2_action( actions, null_fd, STDERR_FILENO );
        if( !stderr_action.has_value() )
        {
            ( void )posix_spawn_file_actions_destroy( &actions );
            ( void )::close( null_fd );
            return grab::fail( stderr_action.error().code,
                               stderr_action.error().message );
        }

        status = posix_spawn_file_actions_addclose( &actions, null_fd );
        if( status != 0 )
        {
            ( void )posix_spawn_file_actions_destroy( &actions );
            ( void )::close( null_fd );
            return grab::fail( grab::ErrorCode::provider_failed,
                               posix_error_message( "posix_spawn addclose", status ) );
        }

        pid_t pid = 0;
        status    = posix_spawnp( &pid,
                                  argv_storage.front().c_str(),
                                  &actions,
                                  nullptr,
                                  argv.data(),
                                  envp.data() );
        ( void )posix_spawn_file_actions_destroy( &actions );
        ( void )::close( null_fd );
        if( status != 0 )
        {
            return grab::fail( grab::ErrorCode::provider_failed,
                               posix_error_message( "posix_spawnp", status ) );
        }
        return pid;
    }

    void
    terminate_app( pid_t pid )
    {
        if( pid <= child_still_running )
        {
            return;
        }

        ( void )::kill( pid, SIGTERM );
        if( wait_for_exit( pid, terminate_grace ) )
        {
            return;
        }

        ( void )::kill( pid, SIGKILL );
        ( void )wait_for_exit( pid, terminate_grace );
    }

}    // namespace grab::inventory
