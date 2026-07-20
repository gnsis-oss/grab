#include "config/batch_manifest.hpp"
#include "config/environment.hpp"
#include "drivers/desktop/x11/config_batch.hpp"
#include "drivers/desktop/x11/workflow.hpp"
#include "grab/config.hpp"
#include "grab/process_ref.hpp"
#include "grab/result.hpp"
#include "grab/screen.hpp"
#include "image/compare_dirs.hpp"
#include "kernel/support/log.hpp"
#include "notify/notifier.hpp"
#include "session/virtual_display.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <expected>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <sys/timerfd.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

extern "C" int
grab_config_batch_set_environment( const char* name,
                                   const char* value,
                                   int         overwrite ) noexcept __asm__( "setenv" );

extern "C" int
grab_config_batch_unset_environment( const char* name ) noexcept __asm__( "unsetenv" );

namespace grab::screen
{
    namespace
    {

        using SteadyClock                            = std::chrono::steady_clock;
        using TimePoint                              = SteadyClock::time_point;

        constexpr int           invalidDescriptor    = -1;
        constexpr int           posixFailure         = -1;
        constexpr int           relativeTimerFlags   = 0;
        constexpr int           overwriteEnvironment = 1;
        // timerfd is Linux-specific; CLOCK_MONOTONIC is Linux UAPI clock id 1.
        constexpr int           linuxMonotonicClockId   = 1;
        constexpr std::uint32_t firstCollisionSuffix    = 2U;
        constexpr std::uint32_t firstFrameNumber        = 1U;
        constexpr std::uint32_t minimumTargetIntervalMs = 20U;
        constexpr std::size_t   frameNumberWidth        = 3U;
        constexpr std::size_t   millisecondWidth        = 3U;
        constexpr std::size_t   utcTimestampBufferSize  = 32U;
        constexpr std::int64_t  millisecondsPerSecond   = 1'000;
        constexpr auto          processProbeTimeout = std::chrono::milliseconds::zero();
        constexpr auto          targetPollInterval  = std::chrono::milliseconds{ 50 };
        constexpr auto          terminationGrace    = std::chrono::seconds{ 2 };
        constexpr std::string_view currentDirectoryName   = "current";
        constexpr std::string_view fallbackProfileStem    = "profile";
        constexpr const char*      displayEnvironmentName = "DISPLAY";
        constexpr std::string_view pngExtension           = ".png";
        constexpr std::string_view notificationApp        = "grab";
        constexpr std::string_view notificationSummary    = "batch capture saved";
        constexpr std::string_view targetErrorSeparator   = "; ";

        [[nodiscard]]
        std::unexpected<grab::Error>
        posix_error( std::string_view operation,
                     int              error_number )
        {
            return grab::fail(
                grab::ErrorCode::InternalFault,
                std::string{ operation } +
                    ": " +
                    std::error_code{ error_number, std::generic_category() }.message()
            );
        }

        [[nodiscard]]
        std::unexpected<grab::Error>
        filesystem_error( std::string_view             operation,
                          const std::filesystem::path& path,
                          const std::error_code&       error )
        {
            return grab::fail(
                grab::ErrorCode::ProviderFailed,
                std::string{ operation } + ": " + path.string() + ": " + error.message()
            );
        }

        [[nodiscard]]
        std::unexpected<grab::Error>
        batch_config_error( const grab::config::Config& cfg,
                            std::string_view            pointer,
                            std::string_view            reason )
        {
            const std::string source =
                cfg.source.empty() ? std::string{ "<config>" } : cfg.source.string();
            return grab::fail(
                grab::ErrorCode::InvalidArgument,
                source + ":" + std::string{ pointer } + ": " + std::string{ reason }
            );
        }

        [[nodiscard]]
        std::mutex&
        display_environment_mutex()
        {
            static std::mutex mutex;
            return mutex;
        }

        class ScopedDisplayEnvironment
        {
            public:

                [[nodiscard]]
                static grab::Result<ScopedDisplayEnvironment>
                activate( std::string_view display )
                {
                    std::unique_lock lock{ display_environment_mutex() };
                    const auto       read_environment = &std::getenv;
                    const char* existing = read_environment( displayEnvironmentName );
                    std::optional<std::string> original;
                    if( existing != nullptr )
                    {
                        original.emplace( existing );
                    }

                    const std::string display_value{ display };
                    const auto set_environment = &::grab_config_batch_set_environment;
                    if( set_environment( displayEnvironmentName,
                                         display_value.c_str(),
                                         overwriteEnvironment ) == posixFailure )
                    {
                        return posix_error( "set DISPLAY", errno );
                    }
                    return ScopedDisplayEnvironment{
                        std::move( lock ),
                        std::move( original )
                    };
                }

                ~ScopedDisplayEnvironment() noexcept
                {
                    const int restore_result = restore_raw();
                    static_cast<void>( restore_result );
                }

                ScopedDisplayEnvironment( const ScopedDisplayEnvironment& ) = delete;
                ScopedDisplayEnvironment&
                operator=( const ScopedDisplayEnvironment& ) = delete;

                ScopedDisplayEnvironment( ScopedDisplayEnvironment&& other ) noexcept :
                    lock_( std::move( other.lock_ ) ),
                    original_( std::move( other.original_ ) ),
                    active_( std::exchange( other.active_,
                                            false ) )
                {
                }

                ScopedDisplayEnvironment&
                operator=( ScopedDisplayEnvironment&& ) = delete;

                [[nodiscard]]
                grab::Result<void>
                restore()
                {
                    if( restore_raw() == posixFailure )
                    {
                        return posix_error( "restore DISPLAY", errno );
                    }
                    return {};
                }

            private:

                ScopedDisplayEnvironment( std::unique_lock<std::mutex> lock,
                                          std::optional<std::string> original ) noexcept
                    :
                    lock_( std::move( lock ) ),
                    original_( std::move( original ) )
                {
                }

                [[nodiscard]]
                int
                restore_raw() noexcept
                {
                    if( !active_ )
                    {
                        return 0;
                    }

                    int result{};
                    if( original_.has_value() )
                    {
                        const auto set_environment =
                            &::grab_config_batch_set_environment;
                        result = set_environment( displayEnvironmentName,
                                                  original_->c_str(),
                                                  overwriteEnvironment );
                    }
                    else
                    {
                        const auto unset_environment =
                            &::grab_config_batch_unset_environment;
                        result = unset_environment( displayEnvironmentName );
                    }
                    if( result != posixFailure )
                    {
                        active_ = false;
                    }
                    return result;
                }

                std::unique_lock<std::mutex> lock_;
                std::optional<std::string>   original_;
                bool                         active_{ true };
        };

        class BatchTimer
        {
            public:

                [[nodiscard]]
                static grab::Result<BatchTimer>
                open()
                {
                    const int descriptor =
                        ::timerfd_create( linuxMonotonicClockId, TFD_CLOEXEC );
                    if( descriptor == invalidDescriptor )
                    {
                        return posix_error( "timerfd_create", errno );
                    }
                    return BatchTimer{ descriptor };
                }

                ~BatchTimer() noexcept
                {
                    if( descriptor_ != invalidDescriptor )
                    {
                        const int close_result = ::close( descriptor_ );
                        static_cast<void>( close_result );
                    }
                }

                BatchTimer( const BatchTimer& ) = delete;
                BatchTimer&
                operator=( const BatchTimer& ) = delete;

                BatchTimer( BatchTimer&& other ) noexcept :
                    descriptor_( std::exchange( other.descriptor_,
                                                invalidDescriptor ) )
                {
                }

                BatchTimer&
                operator=( BatchTimer&& ) = delete;

                [[nodiscard]]
                grab::Result<void>
                wait_for( std::chrono::nanoseconds duration ) const
                {
                    if( duration <= std::chrono::nanoseconds::zero() )
                    {
                        return {};
                    }

                    const auto seconds =
                        std::chrono::duration_cast<std::chrono::seconds>( duration );
                    const auto nanoseconds = duration - seconds;
                    itimerspec timer{};
                    timer.it_value.tv_sec =
                        static_cast<decltype( timer.it_value.tv_sec )>(
                            seconds.count()
                        );
                    timer.it_value.tv_nsec =
                        static_cast<decltype( timer.it_value.tv_nsec )>(
                            nanoseconds.count()
                        );
                    if( ::timerfd_settime( descriptor_,
                                           relativeTimerFlags,
                                           std::addressof( timer ),
                                           nullptr ) == posixFailure )
                    {
                        return posix_error( "timerfd_settime", errno );
                    }

                    std::uint64_t expirations{};
                    for( ;; )
                    {
                        const auto bytes = ::read( descriptor_,
                                                   std::addressof( expirations ),
                                                   sizeof( expirations ) );
                        if( bytes ==
                            static_cast<decltype( bytes )>( sizeof( expirations ) ) )
                        {
                            return {};
                        }
                        if( bytes == posixFailure && errno == EINTR )
                        {
                            continue;
                        }
                        if( bytes == posixFailure )
                        {
                            return posix_error( "read timerfd", errno );
                        }
                        return grab::fail( grab::ErrorCode::InternalFault,
                                           "timerfd returned a short read" );
                    }
                }

            private:

                explicit BatchTimer( int descriptor ) noexcept :
                    descriptor_( descriptor )
                {
                }

                int descriptor_ = invalidDescriptor;
        };

        struct BatchRuntime
        {
                std::optional<grab::screen::VirtualDisplay> virtual_display;
                std::optional<ScopedDisplayEnvironment>     display_environment;
                BatchTimer                                  timer;
                grab::Screen                                screen;
                std::string                                 display_name;
        };

        [[nodiscard]]
        grab::Result<BatchRuntime>
        start_batch_runtime( const grab::config::Config& cfg )
        {
            std::optional<grab::screen::VirtualDisplay> virtual_display;
            std::string                                 display_name;
            if( cfg.display.backend == grab::config::DisplayBackend::Xvfb )
            {
                auto started = grab::screen::VirtualDisplay::start( cfg.display.width,
                                                                    cfg.display.height,
                                                                    cfg.display.depth );
                if( !started.has_value() )
                {
                    return std::unexpected( std::move( started.error() ) );
                }
                display_name = started->display();
                virtual_display.emplace( std::move( *started ) );
            }

            std::optional<ScopedDisplayEnvironment> display_environment;
            if( !display_name.empty() )
            {
                auto activated = ScopedDisplayEnvironment::activate( display_name );
                if( !activated.has_value() )
                {
                    return std::unexpected( std::move( activated.error() ) );
                }
                display_environment.emplace( std::move( *activated ) );
            }

            auto timer = BatchTimer::open();
            if( !timer.has_value() )
            {
                return std::unexpected( std::move( timer.error() ) );
            }
            auto screen =
                grab::Screen::open( display_name.empty() ? nullptr
                                                         : display_name.c_str() );
            if( !screen.has_value() )
            {
                return std::unexpected( std::move( screen.error() ) );
            }
            return BatchRuntime{
                .virtual_display     = std::move( virtual_display ),
                .display_environment = std::move( display_environment ),
                .timer               = std::move( *timer ),
                .screen              = std::move( *screen ),
                .display_name        = std::move( display_name ),
            };
        }

        [[nodiscard]]
        grab::Result<void>
        wait_milliseconds( const BatchTimer& timer,
                           std::uint32_t     milliseconds )
        {
            using Milliseconds = std::chrono::milliseconds;
            const Milliseconds delay{
                static_cast<Milliseconds::rep>( milliseconds ),
            };
            return timer.wait_for(
                std::chrono::duration_cast<std::chrono::nanoseconds>( delay )
            );
        }

        [[nodiscard]]
        grab::Result<void>
        wait_until( const BatchTimer& timer,
                    TimePoint         deadline )
        {
            const TimePoint now = SteadyClock::now();
            if( deadline <= now )
            {
                return {};
            }
            return timer.wait_for(
                std::chrono::ceil<std::chrono::nanoseconds>( deadline - now )
            );
        }

        [[nodiscard]]
        grab::Result<std::tm>
        utc_parts( std::chrono::system_clock::time_point timestamp )
        {
            const auto seconds = std::chrono::floor<std::chrono::seconds>( timestamp );
            const std::time_t raw = std::chrono::system_clock::to_time_t( seconds );
            std::tm           parts{};
            if( ::gmtime_r( std::addressof( raw ), std::addressof( parts ) ) == nullptr )
            {
                return posix_error( "gmtime_r", errno );
            }
            return parts;
        }

        [[nodiscard]]
        grab::Result<std::string>
        format_utc( std::chrono::system_clock::time_point timestamp,
                    const char*                           format )
        {
            auto parts = utc_parts( timestamp );
            if( !parts.has_value() )
            {
                return std::unexpected( std::move( parts.error() ) );
            }
            std::array<char, utcTimestampBufferSize> buffer{};
            if( std::strftime( buffer.data(), buffer.size(), format, &*parts ) == 0U )
            {
                return grab::fail( grab::ErrorCode::InternalFault,
                                   "failed to format UTC timestamp" );
            }
            return std::string{ buffer.data() };
        }

        [[nodiscard]]
        std::string
        zero_padded( std::uint32_t value,
                     std::size_t   width )
        {
            std::string text = std::to_string( value );
            if( text.size() < width )
            {
                text.insert( 0U, width - text.size(), '0' );
            }
            return text;
        }

        [[nodiscard]]
        grab::Result<std::string>
        manifest_timestamp( std::chrono::system_clock::time_point timestamp )
        {
            auto prefix = format_utc( timestamp, "%Y-%m-%dT%H:%M:%S" );
            if( !prefix.has_value() )
            {
                return std::unexpected( std::move( prefix.error() ) );
            }

            const auto milliseconds =
                std::chrono::floor<std::chrono::milliseconds>( timestamp );
            auto remainder =
                milliseconds.time_since_epoch().count() % millisecondsPerSecond;
            if( remainder < 0 )
            {
                remainder += millisecondsPerSecond;
            }
            prefix->push_back( '.' );
            prefix->append( zero_padded( static_cast<std::uint32_t>( remainder ),
                                         millisecondWidth ) );
            prefix->push_back( 'Z' );
            return std::move( *prefix );
        }

        [[nodiscard]]
        grab::Result<std::filesystem::path>
        create_session_directory( const grab::config::Config& cfg,
                                  std::string_view            timestamp )
        {
            if( cfg.batch.output_root.empty() )
            {
                return batch_config_error( cfg,
                                           "/batch/output_root",
                                           "output root is required" );
            }

            std::error_code error;
            static_cast<void>(
                std::filesystem::create_directories( cfg.batch.output_root, error )
            );
            if( error )
            {
                return filesystem_error( "create batch output root",
                                         cfg.batch.output_root,
                                         error );
            }
            if( !std::filesystem::is_directory( cfg.batch.output_root, error ) || error )
            {
                if( error )
                {
                    return filesystem_error( "inspect batch output root",
                                             cfg.batch.output_root,
                                             error );
                }
                return grab::fail( grab::ErrorCode::ProviderFailed,
                                   "batch output root is not a directory: " +
                                       cfg.batch.output_root.string() );
            }

            std::string profile_stem = cfg.source.stem().string();
            if( profile_stem.empty() )
            {
                profile_stem = fallbackProfileStem;
            }
            const std::string base =
                std::string{ timestamp } + "_" + std::move( profile_stem );

            std::uint32_t collision_suffix{};
            for( ;; )
            {
                std::string directory_name = base;
                if( collision_suffix != 0U )
                {
                    directory_name.push_back( '_' );
                    directory_name.append( std::to_string( collision_suffix ) );
                }
                const std::filesystem::path candidate =
                    cfg.batch.output_root / directory_name;

                error.clear();
                const bool created =
                    std::filesystem::create_directory( candidate, error );
                if( created )
                {
                    const std::filesystem::path current =
                        candidate / currentDirectoryName;
                    error.clear();
                    if( !std::filesystem::create_directory( current, error ) )
                    {
                        if( error )
                        {
                            return filesystem_error( "create current capture directory",
                                                     current,
                                                     error );
                        }
                        return grab::fail( grab::ErrorCode::ProviderFailed,
                                           "current capture directory already exists: " +
                                               current.string() );
                    }
                    return candidate;
                }
                if( error && error != std::errc::file_exists )
                {
                    return filesystem_error( "create batch session directory",
                                             candidate,
                                             error );
                }
                error.clear();

                if( collision_suffix == 0U )
                {
                    collision_suffix = firstCollisionSuffix;
                    continue;
                }
                if( collision_suffix == std::numeric_limits<std::uint32_t>::max() )
                {
                    return grab::fail(
                        grab::ErrorCode::ProviderFailed,
                        "batch session directory collision suffix is exhausted"
                    );
                }
                ++collision_suffix;
            }
        }

        [[nodiscard]]
        std::vector<std::string_view>
        string_views( const std::vector<std::string>& strings )
        {
            std::vector<std::string_view> views;
            views.reserve( strings.size() );
            for( const std::string& string : strings )
            {
                views.emplace_back( string );
            }
            return views;
        }

        [[nodiscard]]
        grab::Result<TimePoint>
        target_deadline( double timeout_seconds )
        {
            if( !std::isfinite( timeout_seconds ) || timeout_seconds <= 0.0 )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "target timeout must be finite and positive" );
            }

            const TimePoint now = SteadyClock::now();
            using LongSeconds   = std::chrono::duration<long double>;
            const LongSeconds requested{ static_cast<long double>( timeout_seconds ) };
            const LongSeconds maximum{ TimePoint::max() - now };
            if( requested >= maximum )
            {
                return TimePoint::max();
            }
            return now + std::chrono::duration_cast<SteadyClock::duration>( requested );
        }

        [[nodiscard]]
        grab::Result<grab::config::TargetMatch>
        materialize_match( const grab::config::TargetSpec& target,
                           std::int64_t                    pid )
        {
            grab::config::TargetMatch match;
            match.kind = target.match;
            switch( target.match )
            {
                case grab::config::MatchKind::Pid :
                    if( pid <=
                        0 ||
                        static_cast<std::uint64_t>( pid ) >
                        std::numeric_limits<std::uint32_t>::max() )
                    {
                        return grab::fail( grab::ErrorCode::InternalFault,
                                           "spawned process id cannot be matched" );
                    }
                    match.pid = static_cast<std::uint32_t>( pid );
                    break;
                case grab::config::MatchKind::WmClass :
                case grab::config::MatchKind::Title :
                    if( target.pattern.empty() )
                    {
                        return grab::fail( grab::ErrorCode::InvalidArgument,
                                           "target match pattern is required" );
                    }
                    match.text = target.pattern;
                    break;
                case grab::config::MatchKind::WindowId :
                case grab::config::MatchKind::Count :
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "target match mode is invalid for batch" );
            }
            return match;
        }

        [[nodiscard]]
        grab::Result<std::uint32_t>
        wait_for_window( grab::Screen&                    screen,
                         const grab::config::TargetMatch& match,
                         grab::OwnedProcess&              process,
                         bool&                            process_reaped,
                         const BatchTimer&                timer,
                         TimePoint                        deadline )
        {
            for( ;; )
            {
                auto resolved = grab::screen::resolve_target( screen, match );
                if( resolved.has_value() )
                {
                    return *resolved;
                }
                if( resolved.error().code != grab::ErrorCode::WindowNotFound )
                {
                    return std::unexpected( std::move( resolved.error() ) );
                }

                auto child_status = process.wait( processProbeTimeout );
                if( child_status.has_value() )
                {
                    process_reaped = true;
                    return grab::fail( grab::ErrorCode::WindowNotFound,
                                       "target process exited with status " +
                                           std::to_string( *child_status ) +
                                           " before a matching window appeared" );
                }
                if( child_status.error().code != grab::ErrorCode::DeadlineExceeded )
                {
                    return std::unexpected( std::move( child_status.error() ) );
                }

                const TimePoint now = SteadyClock::now();
                if( now >= deadline )
                {
                    return grab::fail( grab::ErrorCode::DeadlineExceeded,
                                       "timed out waiting for a matching window" );
                }
                const auto      remaining = deadline - now;
                const TimePoint next_poll = remaining <= targetPollInterval
                                              ? deadline
                                              : now + targetPollInterval;
                auto            waited    = wait_until( timer, next_poll );
                if( !waited.has_value() )
                {
                    return std::unexpected( std::move( waited.error() ) );
                }
            }
        }

        void
        append_target_error( grab::config::TargetOutcome& outcome,
                             std::string_view             message )
        {
            if( !outcome.error.empty() )
            {
                outcome.error.append( targetErrorSeparator );
            }
            outcome.error.append( message );
        }

        [[nodiscard]]
        std::filesystem::path
        frame_path( const std::filesystem::path& current,
                    std::string_view             target_name,
                    std::uint32_t                frame_index )
        {
            std::string filename{ target_name };
            if( frame_index != 0U )
            {
                filename.push_back( '_' );
                filename.append( zero_padded( frame_index + firstFrameNumber,
                                              frameNumberWidth ) );
            }
            filename.append( pngExtension );
            return current / filename;
        }

        [[nodiscard]]
        std::int32_t
        popup_timeout( std::uint32_t timeout ) noexcept
        {
            constexpr auto maximum =
                static_cast<std::uint32_t>( std::numeric_limits<std::int32_t>::max() );
            return static_cast<std::int32_t>( std::min( timeout, maximum ) );
        }

        [[nodiscard]]
        grab::notify::Notification
        frame_notification( const std::filesystem::path& path,
                            std::uint32_t                timeout_ms )
        {
            return {
                .app_name   = std::string{ notificationApp },
                .summary    = std::string{ notificationSummary },
                .body       = path.string(),
                .icon       = {},
                .timeout_ms = popup_timeout( timeout_ms ),
            };
        }

        struct TargetExecution
        {
                grab::config::TargetOutcome outcome;
                bool                        failed{};
        };

        void
        fail_target( TargetExecution& execution,
                     std::string_view message )
        {
            execution.failed = true;
            append_target_error( execution.outcome, message );
        }

        void
        cleanup_target_process( const grab::config::TargetSpec& target,
                                grab::OwnedProcess&             process,
                                bool&                           process_reaped,
                                TargetExecution&                execution )
        {
            if( !target.kill_after || process_reaped )
            {
                return;
            }

            auto terminated = process.terminate( terminationGrace );
            if( !terminated.has_value() )
            {
                fail_target( execution,
                             "failed to terminate target process: " +
                                 terminated.error().message );
                return;
            }
            auto status = process.wait( processProbeTimeout );
            if( !status.has_value() )
            {
                fail_target( execution,
                             "failed to reap target process: " +
                                 status.error().message );
                return;
            }
            process_reaped = true;
        }

        [[nodiscard]]
        bool
        cleanup_target_process_best_effort( const grab::config::TargetSpec& target,
                                            grab::OwnedProcess&             process,
                                            bool process_reaped ) noexcept
        {
            try
            {
                if( !target.kill_after || process_reaped )
                {
                    return true;
                }

                auto terminated = process.terminate( terminationGrace );
                if( !terminated.has_value() )
                {
                    return false;
                }
                return process.wait( processProbeTimeout ).has_value();
            }
            catch( ... )
            {
                return false;
            }
        }

        [[nodiscard]]
        grab::Result<grab::OwnedProcess>
        spawn_target_process( const grab::config::TargetSpec& target,
                              std::string_view                display_name )
        {
            std::vector<std::pair<std::string, std::string>> overrides = target.env;
            if( !display_name.empty() )
            {
                overrides.emplace_back( displayEnvironmentName,
                                        std::string{ display_name } );
            }
            const std::vector<std::string> environment =
                grab::config::overlay_environment( overrides );
            const auto argv_views        = string_views( target.argv );
            const auto environment_views = string_views( environment );
            return grab::OwnedProcess::spawn( argv_views, environment_views );
        }

        [[nodiscard]]
        grab::Result<std::uint32_t>
        resolve_target_window( const grab::config::TargetSpec& target,
                               grab::Screen&                   screen,
                               grab::OwnedProcess&             process,
                               bool&                           process_reaped,
                               const BatchTimer&               timer )
        {
            auto match = materialize_match( target, process.id().value );
            if( !match.has_value() )
            {
                return std::unexpected( std::move( match.error() ) );
            }
            auto deadline = target_deadline( target.timeout_s );
            if( !deadline.has_value() )
            {
                return std::unexpected( std::move( deadline.error() ) );
            }
            return wait_for_window( screen,
                                    *match,
                                    process,
                                    process_reaped,
                                    timer,
                                    *deadline );
        }

        [[nodiscard]]
        grab::Result<void>
        capture_target_frames( const grab::config::TargetSpec& target,
                               const grab::config::Config&     cfg,
                               grab::Screen&                   screen,
                               const std::filesystem::path&    current,
                               const BatchTimer&               timer,
                               grab::notify::Notifier*         notifier,
                               grab::config::TargetOutcome&    outcome )
        {
            const bool notifications_enabled = cfg.notifications.enabled &&
                                               cfg.notifications.strategy ==
                                               grab::config::NotifyStrategy::Os;
            for( std::uint32_t frame_index = 0U; frame_index < target.frames;
                 ++frame_index )
            {
                const std::filesystem::path path =
                    frame_path( current, target.name, frame_index );
                auto captured = grab::screen::capture_window_to( screen,
                                                                 outcome.window_id,
                                                                 path.string() );
                if( !captured.has_value() )
                {
                    return std::unexpected( std::move( captured.error() ) );
                }
                outcome.files.push_back( path.filename().string() );

                if( notifications_enabled && notifier != nullptr )
                {
                    auto notified = notifier->notify(
                        frame_notification( path, cfg.notifications.popup_timeout_ms )
                    );
                    if( !notified.has_value() )
                    {
                        return std::unexpected( std::move( notified.error() ) );
                    }
                }
                if( frame_index + firstFrameNumber < target.frames )
                {
                    auto waited = wait_milliseconds( timer, target.interval_ms );
                    if( !waited.has_value() )
                    {
                        return std::unexpected( std::move( waited.error() ) );
                    }
                }
            }
            return {};
        }

        [[nodiscard]]
        TargetExecution
        execute_target( const grab::config::TargetSpec& target,
                        const grab::config::Config&     cfg,
                        grab::Screen&                   screen,
                        std::string_view                display_name,
                        const std::filesystem::path&    current,
                        const BatchTimer&               timer,
                        grab::notify::Notifier*         notifier )
        {
            TargetExecution execution;
            execution.outcome.name = target.name;
            execution.outcome.argv = target.argv;

            if( target.frames == 0U )
            {
                fail_target( execution, "target frames must be positive" );
                return execution;
            }
            auto spawned = spawn_target_process( target, display_name );
            if( !spawned.has_value() )
            {
                fail_target( execution, spawned.error().message );
                return execution;
            }

            grab::OwnedProcess process = std::move( *spawned );
            execution.outcome.pid      = process.id().value;
            bool process_reaped        = false;

            try
            {
                auto window = resolve_target_window( target,
                                                     screen,
                                                     process,
                                                     process_reaped,
                                                     timer );
                if( !window.has_value() )
                {
                    fail_target( execution, window.error().message );
                }
                else
                {
                    execution.outcome.window_id = *window;
                }

                if( !execution.failed )
                {
                    auto settled = wait_milliseconds( timer, target.delay_ms );
                    if( !settled.has_value() )
                    {
                        fail_target( execution, settled.error().message );
                    }
                }

                if( !execution.failed )
                {
                    auto captured = capture_target_frames( target,
                                                           cfg,
                                                           screen,
                                                           current,
                                                           timer,
                                                           notifier,
                                                           execution.outcome );
                    if( !captured.has_value() )
                    {
                        fail_target( execution, captured.error().message );
                    }
                }

                cleanup_target_process( target, process, process_reaped, execution );
            }
            catch( ... )
            {
                static_cast<void>(
                    cleanup_target_process_best_effort( target, process, process_reaped )
                );
                throw;
            }
            return execution;
        }

        void
        increment_saturated( std::uint32_t& value ) noexcept
        {
            if( value != std::numeric_limits<std::uint32_t>::max() )
            {
                ++value;
            }
        }

        [[nodiscard]]
        grab::image::DirCompareMode
        comparison_mode( grab::config::CompareMode mode )
        {
            switch( mode )
            {
                case grab::config::CompareMode::Exact :
                    return grab::image::DirCompareMode::Exact;
                case grab::config::CompareMode::Rmse :
                    return grab::image::DirCompareMode::Rmse;
                case grab::config::CompareMode::Count :
                    return grab::image::DirCompareMode::Count;
            }
            return grab::image::DirCompareMode::Count;
        }

        void
        log_session_started( const std::filesystem::path& session_dir,
                             std::size_t                  target_count )
        {
            grab::log::nominal(
                [&session_dir, target_count]( grab::log::Event& event )
                {
                    event.tag( "config_batch.started" )
                        .value( "session_dir", session_dir.string() )
                        .value( "target_count", target_count );
                }
            );
        }

        void
        log_target_completed( const grab::config::TargetOutcome& outcome )
        {
            grab::log::nominal(
                [&outcome]( grab::log::Event& event )
                {
                    event.tag( "config_batch.target_completed" )
                        .value( "name", outcome.name )
                        .value( "pid", outcome.pid )
                        .value( "window_id", outcome.window_id )
                        .value( "file_count", outcome.files.size() )
                        .value( "failed", !outcome.error.empty() );
                }
            );
        }

        [[nodiscard]]
        grab::Result<void>
        write_failed_manifest( grab::config::BatchManifest& manifest,
                               const std::filesystem::path& session_dir )
        {
            manifest.state = grab::config::RunState::Failed;
            auto ended     = manifest_timestamp( std::chrono::system_clock::now() );
            if( !ended.has_value() )
            {
                return std::unexpected( std::move( ended.error() ) );
            }
            manifest.ended_at = std::move( *ended );
            return manifest.write( session_dir );
        }

        [[nodiscard]]
        bool
        safe_target_name( std::string_view name ) noexcept
        {
            return !name.empty() &&
                   std::ranges::all_of( name,
                                        []( char character )
                                        {
                                            const bool uppercase =
                                                character >= 'A' && character <= 'Z';
                                            const bool lowercase =
                                                character >= 'a' && character <= 'z';
                                            const bool digit =
                                                character >= '0' && character <= '9';
                                            return uppercase ||
                                                   lowercase ||
                                                   digit ||
                                                   character ==
                                                   '.' ||
                                                   character ==
                                                   '_' ||
                                                   character == '-';
                                        } );
        }

        [[nodiscard]]
        std::string
        target_pointer( std::size_t      index,
                        std::string_view field )
        {
            return "/targets/" + std::to_string( index ) + "/" + std::string{ field };
        }

        [[nodiscard]]
        grab::Result<void>
        validate_target( const grab::config::Config&     cfg,
                         const grab::config::TargetSpec& target,
                         std::size_t                     index )
        {
            if( !safe_target_name( target.name ) )
            {
                return batch_config_error( cfg,
                                           target_pointer( index, "name" ),
                                           "target name is not filesystem-safe" );
            }
            if( target.argv.empty() )
            {
                return batch_config_error( cfg,
                                           target_pointer( index, "argv" ),
                                           "target argv must be non-empty" );
            }
            if( target.frames == 0U )
            {
                return batch_config_error( cfg,
                                           target_pointer( index, "frames" ),
                                           "target frames must be positive" );
            }
            if( target.interval_ms < minimumTargetIntervalMs )
            {
                return batch_config_error( cfg,
                                           target_pointer( index, "interval_ms" ),
                                           "target interval must be at least 20 ms" );
            }
            if( !std::isfinite( target.timeout_s ) || target.timeout_s <= 0.0 )
            {
                return batch_config_error(
                    cfg,
                    target_pointer( index, "timeout_s" ),
                    "target timeout must be finite and positive"
                );
            }

            if( target.match == grab::config::MatchKind::Pid )
            {
                if( !target.pattern.empty() )
                {
                    return batch_config_error( cfg,
                                               target_pointer( index, "pattern" ),
                                               "pattern is forbidden for pid matching" );
                }
            }
            else if( target.match ==
                     grab::config::MatchKind::WmClass ||
                     target.match == grab::config::MatchKind::Title )
            {
                if( target.pattern.empty() )
                {
                    return batch_config_error(
                        cfg,
                        target_pointer( index, "pattern" ),
                        "pattern is required for this match mode"
                    );
                }
            }
            else
            {
                return batch_config_error( cfg,
                                           target_pointer( index, "match" ),
                                           "target match mode is invalid for batch" );
            }

            const bool overrides_display =
                std::ranges::any_of( target.env,
                                     []( const auto& entry )
                                     {
                                         return entry.first == displayEnvironmentName;
                                     } );
            if( cfg.display.backend ==
                grab::config::DisplayBackend::Xvfb &&
                overrides_display )
            {
                return batch_config_error( cfg,
                                           target_pointer( index, "env/DISPLAY" ),
                                           "DISPLAY is owned by the xvfb backend" );
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        validate_batch_config( const grab::config::Config& cfg )
        {
            if( cfg.targets.empty() )
            {
                return batch_config_error( cfg,
                                           "/targets",
                                           "batch requires a non-empty target array" );
            }
            if( cfg.display.backend !=
                grab::config::DisplayBackend::Native &&
                cfg.display.backend != grab::config::DisplayBackend::Xvfb )
            {
                return batch_config_error( cfg,
                                           "/display/backend",
                                           "batch display backend is invalid" );
            }
            if( cfg.compare.mode == grab::config::CompareMode::Count )
            {
                return batch_config_error( cfg,
                                           "/compare/mode",
                                           "batch comparison mode is invalid" );
            }
            if( cfg.compare.mode ==
                grab::config::CompareMode::Rmse &&
                ( !std::isfinite( cfg.compare.threshold ) ||
                  cfg.compare.threshold < 0.0 ) )
            {
                return batch_config_error(
                    cfg,
                    "/compare/threshold",
                    "RMSE threshold must be finite and non-negative"
                );
            }
            if( cfg.notifications.strategy == grab::config::NotifyStrategy::Count )
            {
                return batch_config_error( cfg,
                                           "/notifications/strategy",
                                           "batch notification strategy is invalid" );
            }

            for( std::size_t index = 0U; index < cfg.targets.size(); ++index )
            {
                const auto& target       = cfg.targets.at( index );
                auto        valid_target = validate_target( cfg, target, index );
                if( !valid_target.has_value() )
                {
                    return std::unexpected( std::move( valid_target.error() ) );
                }
                for( std::size_t previous = 0U; previous < index; ++previous )
                {
                    if( cfg.targets.at( previous ).name == target.name )
                    {
                        return batch_config_error( cfg,
                                                   target_pointer( index, "name" ),
                                                   "target name must be unique" );
                    }
                }
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<ConfigBatchResult>
        initialize_batch_result( const grab::config::Config& cfg )
        {
            const auto started_at          = std::chrono::system_clock::now();
            auto       directory_timestamp = format_utc( started_at, "%Y%m%dT%H%M%S" );
            if( !directory_timestamp.has_value() )
            {
                return std::unexpected( std::move( directory_timestamp.error() ) );
            }
            auto manifest_started_at = manifest_timestamp( started_at );
            if( !manifest_started_at.has_value() )
            {
                return std::unexpected( std::move( manifest_started_at.error() ) );
            }
            auto session_dir = create_session_directory( cfg, *directory_timestamp );
            if( !session_dir.has_value() )
            {
                return std::unexpected( std::move( session_dir.error() ) );
            }

            ConfigBatchResult result{
                .manifest =
                    grab::config::BatchManifest{
                                                .profile    = cfg.source,
                                                .started_at = std::move( *manifest_started_at ),
                                                .ended_at   = {},
                                                .state      = grab::config::RunState::Running,
                                                .targets    = {},
                                                .compare    = {},
                                                },
                .session_dir      = std::move( *session_dir ),
                .target_errors    = {},
                .compare_failures = {},
            };
            auto written = result.manifest.write( result.session_dir );
            if( !written.has_value() )
            {
                return std::unexpected( std::move( written.error() ) );
            }
            return result;
        }

        [[nodiscard]]
        grab::Result<void>
        execute_targets( const grab::config::Config& cfg,
                         BatchRuntime&               runtime,
                         grab::notify::Notifier*     notifier,
                         ConfigBatchResult&          result )
        {
            const std::filesystem::path current =
                result.session_dir / currentDirectoryName;
            for( const grab::config::TargetSpec& target : cfg.targets )
            {
                TargetExecution execution = execute_target( target,
                                                            cfg,
                                                            runtime.screen,
                                                            runtime.display_name,
                                                            current,
                                                            runtime.timer,
                                                            notifier );
                if( execution.failed )
                {
                    increment_saturated( result.target_errors );
                }
                log_target_completed( execution.outcome );
                result.manifest.targets.push_back( std::move( execution.outcome ) );
                auto written = result.manifest.write( result.session_dir );
                if( !written.has_value() )
                {
                    return std::unexpected( std::move( written.error() ) );
                }
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        compare_current_directory( const grab::config::Config& cfg,
                                   ConfigBatchResult&          result )
        {
            if( !cfg.compare.ref.has_value() )
            {
                return {};
            }
            const std::filesystem::path current =
                result.session_dir / currentDirectoryName;
            auto compared =
                grab::image::compare_dirs( *cfg.compare.ref,
                                           current,
                                           comparison_mode( cfg.compare.mode ),
                                           cfg.compare.threshold );
            if( !compared.has_value() )
            {
                return std::unexpected( std::move( compared.error() ) );
            }

            result.manifest.compare.reserve( compared->size() );
            for( const grab::image::FileCompareResult& entry : *compared )
            {
                result.manifest.compare.push_back( grab::config::FileCompareEntry{
                    .name   = entry.name,
                    .score  = entry.score,
                    .passed = entry.passed,
                } );
                if( !entry.passed )
                {
                    increment_saturated( result.compare_failures );
                }
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        restore_batch_display( BatchRuntime& runtime )
        {
            if( !runtime.display_environment.has_value() )
            {
                return {};
            }
            return runtime.display_environment->restore();
        }

        [[nodiscard]]
        grab::Result<void>
        finish_batch_result( ConfigBatchResult& result )
        {
            result.manifest.state =
                result.target_errors == 0U && result.compare_failures == 0U
                    ? grab::config::RunState::Done
                    : grab::config::RunState::Failed;
            auto ended_at = manifest_timestamp( std::chrono::system_clock::now() );
            if( !ended_at.has_value() )
            {
                return std::unexpected( std::move( ended_at.error() ) );
            }
            result.manifest.ended_at = std::move( *ended_at );
            return result.manifest.write( result.session_dir );
        }

        [[nodiscard]]
        grab::Result<ConfigBatchResult>
        fail_finished_batch( ConfigBatchResult& result,
                             grab::Error        error )
        {
            auto written = write_failed_manifest( result.manifest, result.session_dir );
            if( !written.has_value() )
            {
                return std::unexpected( std::move( written.error() ) );
            }
            return std::unexpected( std::move( error ) );
        }

        void
        log_batch_completed( const ConfigBatchResult& result )
        {
            grab::log::nominal(
                [&result]( grab::log::Event& event )
                {
                    event.tag( "config_batch.completed" )
                        .value( "target_errors", result.target_errors )
                        .value( "compare_failures", result.compare_failures );
                }
            );
        }

    }    // namespace

    grab::Result<ConfigBatchResult>
    run_config_batch( const grab::config::Config& cfg,
                      grab::notify::Notifier*     notifier )
    {
        std::optional<ConfigBatchResult> active_result;
        try
        {
            auto valid = validate_batch_config( cfg );
            if( !valid.has_value() )
            {
                return std::unexpected( std::move( valid.error() ) );
            }
            auto runtime = start_batch_runtime( cfg );
            if( !runtime.has_value() )
            {
                return std::unexpected( std::move( runtime.error() ) );
            }
            auto initialized = initialize_batch_result( cfg );
            if( !initialized.has_value() )
            {
                return std::unexpected( std::move( initialized.error() ) );
            }
            active_result.emplace( std::move( *initialized ) );
            active_result->manifest.targets.reserve( cfg.targets.size() );
            log_session_started( active_result->session_dir, cfg.targets.size() );

            auto targets = execute_targets( cfg, *runtime, notifier, *active_result );
            if( !targets.has_value() )
            {
                return fail_finished_batch( *active_result,
                                            std::move( targets.error() ) );
            }
            auto compared = compare_current_directory( cfg, *active_result );
            if( !compared.has_value() )
            {
                return fail_finished_batch( *active_result,
                                            std::move( compared.error() ) );
            }
            auto restored = restore_batch_display( *runtime );
            if( !restored.has_value() )
            {
                return fail_finished_batch( *active_result,
                                            std::move( restored.error() ) );
            }
            auto finished = finish_batch_result( *active_result );
            if( !finished.has_value() )
            {
                return fail_finished_batch( *active_result,
                                            std::move( finished.error() ) );
            }
            log_batch_completed( *active_result );
            return std::move( *active_result );
        }
        catch( const std::exception& exception )
        {
            grab::Error error{
                .code       = grab::ErrorCode::InternalFault,
                .message    = std::string{ "config batch failed: " } + exception.what(),
                .capability = {},
                .target     = {},
                .attempts   = {},
            };
            if( active_result.has_value() )
            {
                return fail_finished_batch( *active_result, std::move( error ) );
            }
            return std::unexpected( std::move( error ) );
        }
        catch( ... )
        {
            grab::Error error{
                .code       = grab::ErrorCode::InternalFault,
                .message    = "config batch failed with an unknown exception",
                .capability = {},
                .target     = {},
                .attempts   = {},
            };
            if( active_result.has_value() )
            {
                return fail_finished_batch( *active_result, std::move( error ) );
            }
            return std::unexpected( std::move( error ) );
        }
    }

}    // namespace grab::screen
