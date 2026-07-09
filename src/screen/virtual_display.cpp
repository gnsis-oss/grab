#include "grab/result.hpp"
#include "screen/virtual_display.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <fcntl.h>
#include <memory>
// NOLINTNEXTLINE(modernize-deprecated-headers,misc-include-cleaner): POSIX kill(2).
#include <signal.h>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <xcb/xcb.h>

namespace grab::screen
{
    namespace
    {

        constexpr pid_t  kInvalidPid         = static_cast<pid_t>( -1 );
        constexpr int    kXcbOk              = 0;
        constexpr int    kSpawnSuccess       = 0;
        constexpr int    kSystemCallFailed   = -1;
        constexpr int    kPathExistsMode     = F_OK;
        constexpr int    kFirstDisplayNumber = 100;
        constexpr int    kLastDisplayNumber  = 199;
        constexpr int    kNoWaitOptions      = 0;
        constexpr int    kTerminateSignal = SIGTERM;    // NOLINT(misc-include-cleaner)
        constexpr int    kKillSignal      = SIGKILL;    // NOLINT(misc-include-cleaner)
        constexpr mode_t kNoFileMode      = 0;
        constexpr std::size_t kXvfbArgumentCount   = 6U;
        constexpr const char* kDevNullPath         = "/dev/null";
        constexpr const char* kXvfbExecutable      = "Xvfb";
        constexpr const char* kScreenOption        = "-screen";
        constexpr const char* kDefaultScreenNumber = "0";
        constexpr const char* kDisplayLockPrefix   = "/tmp/.X";
        constexpr const char* kDisplayLockSuffix   = "-lock";
        constexpr const char* kDisplaySocketPrefix = "/tmp/.X11-unix/X";
        constexpr char        kDisplayPrefix       = ':';
        constexpr char        kGeometrySeparator   = 'x';
        constexpr auto        kReadyTimeout        = std::chrono::seconds{ 5 };
        constexpr auto        kStopTimeout         = std::chrono::seconds{ 2 };
        constexpr auto        kPollInterval        = std::chrono::milliseconds{ 50 };
        constexpr auto        kInitialExitGrace    = std::chrono::milliseconds{ 100 };

        using XcbConnection =
            std::unique_ptr<xcb_connection_t, decltype( &xcb_disconnect )>;

        class SpawnFileActions
        {
            public:

                SpawnFileActions() noexcept :
                    error_( posix_spawn_file_actions_init( &actions_ ) )
                {
                }

                ~SpawnFileActions()
                {
                    if( error_ == kSpawnSuccess )
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
                int                        error_ = kSpawnSuccess;
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
                return grab::ErrorCode::device_inaccessible;
            }
            return grab::ErrorCode::internal_fault;
        }

        [[nodiscard]]
        bool
        display_connectable( const std::string& display )
        {
            int                 screen_index = 0;
            const XcbConnection connection{
                xcb_connect( display.c_str(), &screen_index ),
                &xcb_disconnect
            };
            return connection !=
                   nullptr &&
                   xcb_connection_has_error( connection.get() ) == kXcbOk;
        }

        [[nodiscard]]
        bool
        path_exists( const std::string& path ) noexcept
        {
            if( access( path.c_str(), kPathExistsMode ) == kSpawnSuccess )
            {
                return true;
            }

            return errno != ENOENT && errno != ENOTDIR;
        }

        [[nodiscard]]
        bool
        display_has_existing_artifact( int display_number )
        {
            const std::string number = std::to_string( display_number );
            return path_exists(
                       std::string{ kDisplayLockPrefix } + number + kDisplayLockSuffix
                   ) ||
                   path_exists( std::string{ kDisplaySocketPrefix } + number );
        }

        [[nodiscard]]
        std::string
        display_name_for( int display_number )
        {
            return std::string{ 1U, kDisplayPrefix } + std::to_string( display_number );
        }

        [[nodiscard]]
        grab::Result<std::string>
        find_free_display()
        {
            for( int display_number = kFirstDisplayNumber;
                 display_number <= kLastDisplayNumber;
                 ++display_number )
            {
                if( display_has_existing_artifact( display_number ) )
                {
                    continue;
                }

                std::string display = display_name_for( display_number );
                if( !display_connectable( display ) )
                {
                    return display;
                }
            }

            return grab::fail( grab::ErrorCode::device_inaccessible,
                               "No free X display found in :100..:199" );
        }

        [[nodiscard]]
        std::string
        screen_geometry( std::uint16_t width,
                         std::uint16_t height,
                         std::uint8_t  depth )
        {
            return std::to_string( width ) +
                   kGeometrySeparator +
                   std::to_string( height ) +
                   kGeometrySeparator +
                   std::to_string( static_cast<unsigned int>( depth ) );
        }

        [[nodiscard]]
        grab::Result<void>
        add_stdio_redirects( SpawnFileActions& actions )
        {
            const int stdin_result = posix_spawn_file_actions_addopen( actions.get(),
                                                                       STDIN_FILENO,
                                                                       kDevNullPath,
                                                                       O_RDONLY,
                                                                       kNoFileMode );
            if( stdin_result != kSpawnSuccess )
            {
                return grab::fail( grab::ErrorCode::internal_fault,
                                   error_message( "posix_spawn stdin redirect",
                                                  stdin_result ) );
            }

            const int stdout_result = posix_spawn_file_actions_addopen( actions.get(),
                                                                        STDOUT_FILENO,
                                                                        kDevNullPath,
                                                                        O_WRONLY,
                                                                        kNoFileMode );
            if( stdout_result != kSpawnSuccess )
            {
                return grab::fail( grab::ErrorCode::internal_fault,
                                   error_message( "posix_spawn stdout redirect",
                                                  stdout_result ) );
            }

            const int stderr_result = posix_spawn_file_actions_addopen( actions.get(),
                                                                        STDERR_FILENO,
                                                                        kDevNullPath,
                                                                        O_WRONLY,
                                                                        kNoFileMode );
            if( stderr_result != kSpawnSuccess )
            {
                return grab::fail( grab::ErrorCode::internal_fault,
                                   error_message( "posix_spawn stderr redirect",
                                                  stderr_result ) );
            }

            return {};
        }

        [[nodiscard]]
        grab::Result<pid_t>
        spawn_xvfb( const std::string& display,
                    const std::string& geometry )
        {
            SpawnFileActions actions;
            if( actions.error() != kSpawnSuccess )
            {
                return grab::fail( grab::ErrorCode::internal_fault,
                                   error_message( "posix_spawn_file_actions_init",
                                                  actions.error() ) );
            }

            auto redirects = add_stdio_redirects( actions );
            if( !redirects.has_value() )
            {
                return std::unexpected( std::move( redirects.error() ) );
            }

            std::string                           executable{ kXvfbExecutable };
            std::string                           screen_option{ kScreenOption };
            std::string                           screen_number{ kDefaultScreenNumber };
            std::string                           display_argument{ display };
            std::string                           geometry_argument{ geometry };
            std::array<char*, kXvfbArgumentCount> arguments{
                executable.data(),
                display_argument.data(),
                screen_option.data(),
                screen_number.data(),
                geometry_argument.data(),
                nullptr,
            };

            pid_t     child_pid    = kInvalidPid;
            const int spawn_result = posix_spawnp( &child_pid,
                                                   executable.c_str(),
                                                   actions.get(),
                                                   nullptr,
                                                   arguments.data(),
                                                   environ );
            if( spawn_result != kSpawnSuccess )
            {
                return grab::fail( spawn_error_code( spawn_result ),
                                   error_message( "posix_spawnp Xvfb", spawn_result ) );
            }

            return child_pid;
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
                    return grab::fail( grab::ErrorCode::internal_fault,
                                       "Xvfb exited before accepting connections: " +
                                           wait_status_message( status ) );
                }
                if( errno == EINTR )
                {
                    continue;
                }
                if( errno == ECHILD )
                {
                    return grab::fail( grab::ErrorCode::internal_fault,
                                       "Xvfb child process is not waitable" );
                }
                return grab::fail( grab::ErrorCode::internal_fault,
                                   error_message( "waitpid", errno ) );
            }
        }

        [[nodiscard]]
        grab::Result<void>
        wait_until_ready( pid_t              child_pid,
                          const std::string& display )
        {
            const auto start_time       = std::chrono::steady_clock::now();
            const auto first_exit_check = start_time + kInitialExitGrace;
            const auto deadline         = start_time + kReadyTimeout;
            auto       now              = start_time;
            while( now < deadline )
            {
                if( display_connectable( display ) )
                {
                    return {};
                }

                if( now >= first_exit_check )
                {
                    auto child_running = fail_if_child_exited( child_pid );
                    if( !child_running.has_value() )
                    {
                        return std::unexpected( std::move( child_running.error() ) );
                    }
                }

                std::this_thread::sleep_for( kPollInterval );
                now = std::chrono::steady_clock::now();
            }

            auto child_running = fail_if_child_exited( child_pid );
            if( !child_running.has_value() )
            {
                return std::unexpected( std::move( child_running.error() ) );
            }

            return grab::fail( grab::ErrorCode::device_inaccessible,
                               "Xvfb display did not become connectable" );
        }

        void
        wait_for_child_after_signal( pid_t child_pid ) noexcept
        {
            const auto deadline = std::chrono::steady_clock::now() + kStopTimeout;
            while( std::chrono::steady_clock::now() < deadline )
            {
                int         status      = 0;
                const pid_t wait_result = waitpid(
                    child_pid,
                    &status,
                    WNOHANG    // NOLINT(misc-include-cleaner)
                );
                if( wait_result == child_pid )
                {
                    return;
                }
                if( wait_result == kSystemCallFailed && errno == ECHILD )
                {
                    return;
                }
                if( wait_result == kSystemCallFailed && errno == EINTR )
                {
                    continue;
                }
                std::this_thread::sleep_for( kPollInterval );
            }

            // NOLINTNEXTLINE(misc-include-cleaner): provided by POSIX <signal.h>.
            if( kill( child_pid, kKillSignal ) == kSystemCallFailed && errno == ESRCH )
            {
                return;
            }

            for( ;; )
            {
                int         status      = 0;
                const pid_t wait_result = waitpid( child_pid, &status, kNoWaitOptions );
                if( wait_result == child_pid )
                {
                    return;
                }
                if( wait_result == kSystemCallFailed && errno == EINTR )
                {
                    continue;
                }
                return;
            }
        }

        void
        terminate_child( pid_t child_pid ) noexcept
        {
            if( child_pid == kInvalidPid )
            {
                return;
            }

            // NOLINTNEXTLINE(misc-include-cleaner): provided by POSIX <signal.h>.
            if( kill( child_pid, kTerminateSignal ) ==
                kSystemCallFailed &&
                errno == ESRCH )
            {
                return;
            }

            wait_for_child_after_signal( child_pid );
        }

    }    // namespace

    VirtualDisplay::VirtualDisplay( pid_t       child_pid,
                                    std::string display ) noexcept :
        child_pid_( child_pid ),
        display_( std::move( display ) )
    {
    }

    VirtualDisplay::~VirtualDisplay()
    {
        stop();
    }

    VirtualDisplay::VirtualDisplay( VirtualDisplay&& other ) noexcept :
        child_pid_( std::exchange( other.child_pid_,
                                   kInvalidPid ) ),
        display_( std::move( other.display_ ) )
    {
    }

    VirtualDisplay&
    VirtualDisplay::operator=( VirtualDisplay&& other ) noexcept
    {
        if( this != &other )
        {
            stop();
            child_pid_ = std::exchange( other.child_pid_, kInvalidPid );
            display_   = std::move( other.display_ );
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
            return grab::fail( grab::ErrorCode::invalid_argument,
                               "Virtual display dimensions and depth must be non-zero" );
        }

        auto display = find_free_display();
        if( !display.has_value() )
        {
            return std::unexpected( std::move( display.error() ) );
        }

        auto child_pid = spawn_xvfb( *display, screen_geometry( width, height, depth ) );
        if( !child_pid.has_value() )
        {
            return std::unexpected( std::move( child_pid.error() ) );
        }

        auto ready = wait_until_ready( *child_pid, *display );
        if( !ready.has_value() )
        {
            terminate_child( *child_pid );
            return std::unexpected( std::move( ready.error() ) );
        }

        return VirtualDisplay{ *child_pid, std::move( *display ) };
    }

    const std::string&
    VirtualDisplay::display() const noexcept
    {
        return display_;
    }

    void
    VirtualDisplay::stop() noexcept
    {
        terminate_child( child_pid_ );
        child_pid_ = kInvalidPid;
        display_.clear();
    }

}    // namespace grab::screen
