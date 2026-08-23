#include "grab/process_ref.hpp"
#include "grab/result.hpp"
#include "session/display_probe.hpp"
#include "session/poll_wait.hpp"
#include "session/virtual_display.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <fcntl.h>
#include <optional>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace grab::screen
{
    namespace
    {

        // NOLINTNEXTLINE(misc-include-cleaner): provided by POSIX <sys/types.h>.
        constexpr pid_t       invalidPid        = static_cast<pid_t>( -1 );
        constexpr int         spawnSuccess      = 0;
        constexpr mode_t      noFileMode        = 0;
        constexpr std::size_t xvfbArgumentCount = 7U;
        constexpr const char* devNullPath       = "/dev/null";
        constexpr const char* xvfbExecutable    = "Xvfb";
        constexpr const char* screenOption      = "-screen";
        // The readiness probe connects and disconnects; without -noreset that
        // last-client disconnect resets the server and races later connects.
        constexpr const char* noResetOption       = "-noreset";
        constexpr const char* defaultScreenNumber = "0";
        constexpr char        geometrySeparator   = 'x';
        constexpr auto        readyTimeout        = std::chrono::seconds{ 5 };
        constexpr auto        stopTimeout         = std::chrono::seconds{ 2 };

        class SpawnFileActions
        {
            public:

                SpawnFileActions() noexcept :
                    error_( posix_spawn_file_actions_init( &actions_ ) )
                {
                }

                ~SpawnFileActions()
                {
                    if( error_ == spawnSuccess )
                    {
                        static_cast<void>(
                            posix_spawn_file_actions_destroy( &actions_ )
                        );
                    }
                }

                SpawnFileActions( const SpawnFileActions& ) = delete;
                SpawnFileActions&
                operator=( const SpawnFileActions& )   = delete;
                SpawnFileActions( SpawnFileActions&& ) = delete;
                SpawnFileActions&
                operator=( SpawnFileActions&& ) = delete;

                [[nodiscard]]
                int
                error() const noexcept
                {
                    return error_;
                }

                [[nodiscard]]
                posix_spawn_file_actions_t*
                get() noexcept
                {
                    return &actions_;
                }

            private:

                posix_spawn_file_actions_t actions_{};
                int                        error_ = spawnSuccess;
        };

        [[nodiscard]]
        std::string
        error_message( std::string_view operation,
                       int              error_number )
        {
            return std::string{ operation } +
                   " failed: " +
                   std::error_code{ error_number, std::generic_category() }.message();
        }

        [[nodiscard]]
        grab::ErrorCode
        spawn_error_code( int error_number ) noexcept
        {
            if( error_number == ENOENT || error_number == EACCES )
            {
                return grab::ErrorCode::DeviceInaccessible;
            }
            return grab::ErrorCode::InternalFault;
        }

        [[nodiscard]]
        std::string
        screen_geometry( std::uint16_t width,
                         std::uint16_t height,
                         std::uint8_t  depth )
        {
            return std::to_string( width ) +
                   geometrySeparator +
                   std::to_string( height ) +
                   geometrySeparator +
                   std::to_string( static_cast<unsigned int>( depth ) );
        }

        [[nodiscard]]
        grab::Result<void>
        add_stdio_redirects( SpawnFileActions& actions )
        {
            const int stdin_result = posix_spawn_file_actions_addopen( actions.get(),
                                                                       STDIN_FILENO,
                                                                       devNullPath,
                                                                       O_RDONLY,
                                                                       noFileMode );
            if( stdin_result != spawnSuccess )
            {
                return grab::fail( grab::ErrorCode::InternalFault,
                                   error_message( "posix_spawn stdin redirect",
                                                  stdin_result ) );
            }

            const int stdout_result = posix_spawn_file_actions_addopen( actions.get(),
                                                                        STDOUT_FILENO,
                                                                        devNullPath,
                                                                        O_WRONLY,
                                                                        noFileMode );
            if( stdout_result != spawnSuccess )
            {
                return grab::fail( grab::ErrorCode::InternalFault,
                                   error_message( "posix_spawn stdout redirect",
                                                  stdout_result ) );
            }

            const int stderr_result = posix_spawn_file_actions_addopen( actions.get(),
                                                                        STDERR_FILENO,
                                                                        devNullPath,
                                                                        O_WRONLY,
                                                                        noFileMode );
            if( stderr_result != spawnSuccess )
            {
                return grab::fail( grab::ErrorCode::InternalFault,
                                   error_message( "posix_spawn stderr redirect",
                                                  stderr_result ) );
            }

            return {};
        }

        [[nodiscard]]
        grab::Result<grab::OwnedProcess>
        spawn_xvfb( const std::string& display,
                    const std::string& geometry )
        {
            SpawnFileActions actions;
            if( actions.error() != spawnSuccess )
            {
                return grab::fail( grab::ErrorCode::InternalFault,
                                   error_message( "posix_spawn_file_actions_init",
                                                  actions.error() ) );
            }

            auto redirects = add_stdio_redirects( actions );
            if( !redirects.has_value() )
            {
                return std::unexpected( std::move( redirects.error() ) );
            }

            std::string                          executable{ xvfbExecutable };
            std::string                          screen_option{ screenOption };
            std::string                          screen_number{ defaultScreenNumber };
            std::string                          display_argument{ display };
            std::string                          geometry_argument{ geometry };
            std::string                          no_reset_option{ noResetOption };
            std::array<char*, xvfbArgumentCount> arguments{
                executable.data(),
                display_argument.data(),
                no_reset_option.data(),
                screen_option.data(),
                screen_number.data(),
                geometry_argument.data(),
                nullptr,
            };

            pid_t     child_pid    = invalidPid;
            const int spawn_result = posix_spawnp( &child_pid,
                                                   executable.c_str(),
                                                   actions.get(),
                                                   nullptr,
                                                   arguments.data(),
                                                   environ );
            if( spawn_result != spawnSuccess )
            {
                return grab::fail( spawn_error_code( spawn_result ),
                                   error_message( "posix_spawnp Xvfb", spawn_result ) );
            }

            auto child = grab::OwnedProcess::adopt_child( child_pid );
            if( !child.has_value() )
            {
                return std::unexpected( std::move( child.error() ) );
            }
            return child;
        }

        [[nodiscard]]
        std::string
        wait_status_message( int status )
        {
            // NOLINTNEXTLINE(misc-include-cleaner): provided by POSIX <sys/wait.h>.
            if( WIFEXITED( status ) )
            {
                // NOLINTNEXTLINE(misc-include-cleaner): provided by POSIX <sys/wait.h>.
                return "exited with status " + std::to_string( WEXITSTATUS( status ) );
            }
            // NOLINTNEXTLINE(misc-include-cleaner): provided by POSIX <sys/wait.h>.
            if( WIFSIGNALED( status ) )
            {
                // NOLINTNEXTLINE(misc-include-cleaner): provided by POSIX <sys/wait.h>.
                return "terminated by signal " + std::to_string( WTERMSIG( status ) );
            }
            return "changed state with wait status " + std::to_string( status );
        }

        [[nodiscard]]
        grab::Result<void>
        fail_if_child_exited( pid_t child_pid )
        {
            int status = 0;
            for( ;; )
            {
                const pid_t wait_result =
                    waitpid( child_pid,
                             &status,
                             WNOHANG );    // NOLINT(misc-include-cleaner)
                if( wait_result == 0 )
                {
                    return {};
                }
                if( wait_result == child_pid )
                {
                    return grab::fail( grab::ErrorCode::InternalFault,
                                       "Xvfb exited before accepting connections: " +
                                           wait_status_message( status ) );
                }
                if( errno == EINTR )
                {
                    continue;
                }
                if( errno == ECHILD )
                {
                    return grab::fail( grab::ErrorCode::InternalFault,
                                       "Xvfb child process is not waitable" );
                }
                return grab::fail( grab::ErrorCode::InternalFault,
                                   error_message( "waitpid", errno ) );
            }
        }

        // Readiness is the display accepting a connection, with an exit check
        // in the same loop so a server that dies on startup is reported as
        // what happened to it rather than as a timeout.
        [[nodiscard]]
        grab::Result<void>
        wait_until_ready( pid_t              child_pid,
                          const std::string& display )
        {
            grab::Result<void> exit_reason;
            const auto         outcome = grab::session::poll_until(
                [child_pid, &display, &exit_reason]() -> grab::session::Probe
                {
                    if( grab::session::display_connectable( display ) )
                    {
                        return grab::session::Probe::Ready;
                    }
                    exit_reason = fail_if_child_exited( child_pid );
                    return exit_reason.has_value() ? grab::session::Probe::Retry
                                                   : grab::session::Probe::Abandoned;
                },
                std::chrono::duration_cast<std::chrono::milliseconds>( readyTimeout )
            );
            if( outcome == grab::session::Probe::Ready )
            {
                return {};
            }
            if( !exit_reason.has_value() )
            {
                return exit_reason;
            }
            return grab::fail( grab::ErrorCode::DeviceInaccessible,
                               "Xvfb display did not become connectable" );
        }

    }    // namespace

    VirtualDisplay::VirtualDisplay( grab::OwnedProcess child,
                                    std::string        display ) noexcept :
        child_( std::move( child ) ),
        display_( std::move( display ) )
    {
    }

    VirtualDisplay::~VirtualDisplay()
    {
        stop();
    }

    VirtualDisplay::VirtualDisplay( VirtualDisplay&& other ) noexcept :
        child_( std::exchange( other.child_,
                               std::nullopt ) ),
        display_( std::move( other.display_ ) )
    {
    }

    VirtualDisplay&
    VirtualDisplay::operator=( VirtualDisplay&& other ) noexcept
    {
        if( this != &other )
        {
            stop();
            child_   = std::exchange( other.child_, std::nullopt );
            display_ = std::move( other.display_ );
        }
        return *this;
    }

    grab::Result<VirtualDisplay>
    VirtualDisplay::start( std::uint16_t width,
                           std::uint16_t height,
                           std::uint8_t  depth )
    {
        if( width == 0U || height == 0U || depth == 0U )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "Virtual display dimensions and depth must be non-zero" );
        }

        auto display = grab::session::find_free_display();
        if( !display.has_value() )
        {
            return std::unexpected( std::move( display.error() ) );
        }

        auto child = spawn_xvfb( *display, screen_geometry( width, height, depth ) );
        if( !child.has_value() )
        {
            return std::unexpected( std::move( child.error() ) );
        }

        const auto child_pid = static_cast<pid_t>( child->id().value );
        auto       ready     = wait_until_ready( child_pid, *display );
        if( !ready.has_value() )
        {
            auto terminate_result = child->terminate( stopTimeout );
            static_cast<void>( terminate_result );
            return std::unexpected( std::move( ready.error() ) );
        }

        return VirtualDisplay{ std::move( *child ), std::move( *display ) };
    }

    const std::string&
    VirtualDisplay::display() const noexcept
    {
        return display_;
    }

    void
    VirtualDisplay::stop() noexcept
    {
        if( child_.has_value() )
        {
            auto terminate_result = child_->terminate( stopTimeout );
            static_cast<void>( terminate_result );
            child_.reset();
        }
        display_.clear();
    }

}    // namespace grab::screen
