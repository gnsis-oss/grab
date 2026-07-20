#include "codec/png.hpp"
#include "config/pattern.hpp"
#include "config/rotation.hpp"
#include "config/schedule.hpp"
#include "drivers/desktop/x11/config_watch.hpp"
#include "drivers/desktop/x11/enumerate.hpp"
#include "drivers/desktop/x11/window_match.hpp"
#include "drivers/desktop/x11/workflow.hpp"
#include "grab/config.hpp"
#include "grab/input.hpp"
#include "grab/result.hpp"
#include "grab/screen.hpp"
#include "kernel/support/log.hpp"
#include "kernel/support/posix_mkstemp.hpp"
#include "notify/notifier.hpp"
#include "session/virtual_display.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/eventfd.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

extern "C" int
grab_config_watch_set_environment( const char* name,
                                   const char* value,
                                   int         overwrite ) noexcept __asm__( "setenv" );

extern "C" int
grab_config_watch_unset_environment( const char* name ) noexcept __asm__( "unsetenv" );

extern "C" int
grab_config_watch_set_descriptor_flags( int descriptor,
                                        int command,
                                        int flags ) __asm__( "fcntl" );

namespace grab::screen
{
    namespace
    {

        constexpr int              invalidDescriptor   = -1;
        constexpr int              posixFailure        = -1;
        constexpr int              infinitePollTimeout = -1;
        constexpr int              relativeTimerFlags  = 0;
        constexpr int              noEvents            = 0;
        // timerfd is Linux-specific; CLOCK_MONOTONIC is Linux UAPI clock id 1.
        constexpr int              linuxMonotonicClockId  = 1;
        constexpr std::size_t      timerDescriptorIndex   = 0U;
        constexpr std::size_t      stopDescriptorIndex    = 1U;
        constexpr std::size_t      pollDescriptorCount    = 2U;
        constexpr std::size_t      pngExtensionLength     = 4U;
        constexpr std::uint32_t    minimumWatchIntervalMs = 20U;
        constexpr std::uint64_t    wakeEventValue         = 1U;
        constexpr int              overwriteEnvironment   = 1;
        constexpr auto             agePruneInterval       = std::chrono::hours{ 1 };
        constexpr auto             diskRefreshInterval    = std::chrono::minutes{ 1 };
        constexpr const char*      displayEnvironmentName = "DISPLAY";
        constexpr std::string_view statusTimestampPattern = "{timestamp}.png";
        constexpr std::string_view pngFormat              = "png";
        constexpr std::string_view pngExtension           = ".png";
        constexpr std::string_view notificationApp        = "grab";
        constexpr std::string_view captureSummary         = "watch capture saved";
        constexpr std::string_view pauseSummary           = "watch capture paused";
        constexpr std::string_view pauseBody            = "rotation disk limit reached";
        constexpr std::string_view stagingFilename      = ".grab-watch-staging-XXXXXX";
        constexpr std::string_view descriptorPathPrefix = "/proc/self/fd/";
        constexpr std::string_view parentComponent      = "..";
        constexpr std::string_view currentComponent     = ".";

        [[nodiscard]]
        std::unexpected<grab::Error>
        posix_error( std::string_view operation,
                     int              error_number );

        template<typename TimerSpecification,
                 typename Seconds,
                 typename Nanoseconds>
        [[nodiscard]]
        int
        set_relative_timer( int         ( *set_time )( int,
                                                       int,
                                                       const TimerSpecification*,
                                                       TimerSpecification* ) noexcept,
                            int         descriptor,
                            int         flags,
                            Seconds     seconds,
                            Nanoseconds nanoseconds )
        {
            TimerSpecification timer{};
            timer.it_value.tv_sec =
                static_cast<decltype( timer.it_value.tv_sec )>( seconds );
            timer.it_value.tv_nsec =
                static_cast<decltype( timer.it_value.tv_nsec )>( nanoseconds );
            return set_time( descriptor, flags, &timer, nullptr );
        }

        class ScopedDescriptor
        {
            public:

                explicit ScopedDescriptor( int descriptor ) noexcept :
                    descriptor_( descriptor )
                {
                }

                ~ScopedDescriptor() noexcept
                {
                    close();
                }

                ScopedDescriptor( const ScopedDescriptor& ) = delete;
                ScopedDescriptor&
                operator=( const ScopedDescriptor& )   = delete;
                ScopedDescriptor( ScopedDescriptor&& ) = delete;
                ScopedDescriptor&
                operator=( ScopedDescriptor&& ) = delete;

                [[nodiscard]]
                int
                get() const noexcept
                {
                    return descriptor_;
                }

                [[nodiscard]]
                int
                release() noexcept
                {
                    return std::exchange( descriptor_, invalidDescriptor );
                }

            private:

                void
                close() noexcept
                {
                    if( descriptor_ == invalidDescriptor )
                    {
                        return;
                    }
                    const int close_result = ::close( descriptor_ );
                    static_cast<void>( close_result );
                    descriptor_ = invalidDescriptor;
                }

                int descriptor_ = invalidDescriptor;
        };

        class ScopedDisplayEnvironment
        {
            public:

                [[nodiscard]]
                static grab::Result<ScopedDisplayEnvironment>
                activate( std::string_view display )
                {
                    const auto  read_environment = &std::getenv;
                    const char* existing = read_environment( displayEnvironmentName );
                    std::optional<std::string> original;
                    if( existing != nullptr )
                    {
                        original.emplace( existing );
                    }

                    const std::string display_value{ display };
                    const auto set_environment = &::grab_config_watch_set_environment;
                    if( set_environment( displayEnvironmentName,
                                         display_value.c_str(),
                                         overwriteEnvironment ) == posixFailure )
                    {
                        return posix_error( "set DISPLAY", errno );
                    }
                    return ScopedDisplayEnvironment{ std::move( original ) };
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

                explicit ScopedDisplayEnvironment(
                    std::optional<std::string> original
                ) noexcept :
                    original_( std::move( original ) )
                {
                }

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
                            &::grab_config_watch_set_environment;
                        result = set_environment( displayEnvironmentName,
                                                  original_->c_str(),
                                                  overwriteEnvironment );
                    }
                    else
                    {
                        const auto unset_environment =
                            &::grab_config_watch_unset_environment;
                        result = unset_environment( displayEnvironmentName );
                    }
                    if( result != posixFailure )
                    {
                        active_ = false;
                    }
                    return result;
                }

                std::optional<std::string> original_;
                bool                       active_{ true };
        };

        [[nodiscard]]
        std::mutex&
        display_environment_mutex()
        {
            static std::mutex mutex;
            return mutex;
        }

        template<typename Operation>
        [[nodiscard]]
        auto
        with_ambient_display( std::string_view display,
                              Operation&&      operation )
            -> decltype( std::forward<Operation>( operation )() )
        {
            const std::scoped_lock lock( display_environment_mutex() );
            auto environment = ScopedDisplayEnvironment::activate( display );
            if( !environment.has_value() )
            {
                return std::unexpected( std::move( environment.error() ) );
            }
            auto result   = std::forward<Operation>( operation )();
            auto restored = environment->restore();
            if( !restored.has_value() )
            {
                return std::unexpected( std::move( restored.error() ) );
            }
            return result;
        }

        [[nodiscard]]
        std::optional<std::string>
        ambient_display_name()
        {
            const std::scoped_lock lock( display_environment_mutex() );
            const auto             read_environment = &std::getenv;
            const char*            value = read_environment( displayEnvironmentName );
            if( value != nullptr )
            {
                return std::string{ value };
            }
            return std::nullopt;
        }

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
        bool
        is_missing( const std::error_code& error ) noexcept
        {
            return error == std::errc::no_such_file_or_directory;
        }

        [[nodiscard]]
        grab::Result<void>
        ensure_real_directory( const std::filesystem::path& path )
        {
            std::error_code error;
            auto            status = std::filesystem::symlink_status( path, error );
            const bool      missing =
                ( error && is_missing( error ) ) ||
                ( !error && status.type() == std::filesystem::file_type::not_found );
            if( error && !missing )
            {
                return filesystem_error( "inspect output directory", path, error );
            }
            if( missing )
            {
                error.clear();
                static_cast<void>( std::filesystem::create_directory( path, error ) );
                if( error )
                {
                    return filesystem_error( "create output directory", path, error );
                }
                status = std::filesystem::symlink_status( path, error );
                if( error )
                {
                    return filesystem_error( "verify output directory", path, error );
                }
            }
            if( !std::filesystem::is_directory( status ) ||
                std::filesystem::is_symlink( status ) )
            {
                return grab::fail(
                    grab::ErrorCode::ProviderFailed,
                    "watch output path component is not a real directory: " +
                        path.string()
                );
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        ensure_real_directory_chain( const std::filesystem::path& path )
        {
            if( !path.is_absolute() )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "watch output directory must be absolute" );
            }

            std::filesystem::path current = path.root_path();
            auto                  ensured = ensure_real_directory( current );
            if( !ensured.has_value() )
            {
                return ensured;
            }
            for( const std::filesystem::path& component : path.relative_path() )
            {
                const std::string component_text = component.generic_string();
                if( component_text == currentComponent )
                {
                    continue;
                }
                if( component_text == parentComponent )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "watch output directory contains '..'" );
                }
                current /= component;
                ensured  = ensure_real_directory( current );
                if( !ensured.has_value() )
                {
                    return ensured;
                }
            }
            return {};
        }

        [[nodiscard]]
        bool
        is_descendant_or_same( const std::filesystem::path& root,
                               const std::filesystem::path& candidate )
        {
            const std::filesystem::path relative = candidate.lexically_relative( root );
            if( relative.empty() || relative.is_absolute() )
            {
                return false;
            }
            return std::ranges::all_of( relative,
                                        []( const std::filesystem::path& component )
                                        {
                                            return component.generic_string() !=
                                                   parentComponent;
                                        } );
        }

        [[nodiscard]]
        grab::Result<void>
        validate_capture_directory( const std::filesystem::path& output,
                                    const std::filesystem::path& directory )
        {
            if( !is_descendant_or_same( output, directory ) )
            {
                return grab::fail(
                    grab::ErrorCode::InvalidArgument,
                    "watch capture path is outside the output directory: " +
                        directory.string()
                );
            }
            return ensure_real_directory_chain( directory );
        }

        [[nodiscard]]
        grab::Result<void>
        validate_publish_destination( const std::filesystem::path& path )
        {
            std::error_code error;
            const auto      status = std::filesystem::symlink_status( path, error );
            if( ( error && is_missing( error ) ) ||
                ( !error && status.type() == std::filesystem::file_type::not_found ) )
            {
                return {};
            }
            if( error )
            {
                return filesystem_error( "inspect capture destination", path, error );
            }
            if( !std::filesystem::is_regular_file( status ) ||
                std::filesystem::is_symlink( status ) )
            {
                return grab::fail(
                    grab::ErrorCode::ProviderFailed,
                    "watch capture destination is not a real regular file: " +
                        path.string()
                );
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        validate_staged_file( const std::filesystem::path& path,
                              int                          descriptor )
        {
            struct stat descriptor_status{};
            if( ::fstat( descriptor, std::addressof( descriptor_status ) ) ==
                posixFailure )
            {
                return posix_error( "fstat staged watch capture", errno );
            }
            struct stat path_status{};
            if( ::lstat( path.c_str(), std::addressof( path_status ) ) == posixFailure )
            {
                return posix_error( "lstat staged watch capture", errno );
            }
            if( S_ISREG( descriptor_status.st_mode ) ==
                0 ||
                S_ISREG( path_status.st_mode ) ==
                0 ||
                descriptor_status.st_dev !=
                path_status.st_dev ||
                descriptor_status.st_ino != path_status.st_ino )
            {
                return grab::fail(
                    grab::ErrorCode::ProviderFailed,
                    "staged watch capture path no longer names its reserved file: " +
                        path.string()
                );
            }
            return {};
        }

        void
        remove_staged_file( const std::filesystem::path& path,
                            int                          descriptor ) noexcept
        {
            struct stat descriptor_status{};
            struct stat path_status{};
            if( descriptor !=
                invalidDescriptor &&
                ::fstat( descriptor, std::addressof( descriptor_status ) ) !=
                posixFailure &&
                ::lstat( path.c_str(), std::addressof( path_status ) ) !=
                posixFailure &&
                S_ISREG( descriptor_status.st_mode ) !=
                0 &&
                S_ISREG( path_status.st_mode ) !=
                0 &&
                descriptor_status.st_dev ==
                path_status.st_dev &&
                descriptor_status.st_ino == path_status.st_ino )
            {
                const int unlink_result = ::unlink( path.c_str() );
                static_cast<void>( unlink_result );
            }
        }

        class TemporaryCapturePath
        {
            public:

                [[nodiscard]]
                static grab::Result<TemporaryCapturePath>
                create( const std::filesystem::path& directory )
                {
                    std::string path_text = ( directory / stagingFilename ).string();
                    const int   descriptor =
                        grab::core::posix::mkstemp( path_text.data() );
                    if( descriptor == invalidDescriptor )
                    {
                        return posix_error( "mkstemp watch capture", errno );
                    }
                    if( ::grab_config_watch_set_descriptor_flags( descriptor,
                                                                  F_SETFD,
                                                                  FD_CLOEXEC ) ==
                        posixFailure )
                    {
                        const int flags_error = errno;
                        remove_staged_file( path_text, descriptor );
                        const int close_result = ::close( descriptor );
                        static_cast<void>( close_result );
                        return posix_error( "set staging file close-on-exec",
                                            flags_error );
                    }
                    return TemporaryCapturePath{
                        std::filesystem::path{ std::move( path_text ) },
                        descriptor,
                    };
                }

                ~TemporaryCapturePath() noexcept
                {
                    discard();
                    close_descriptor();
                }

                TemporaryCapturePath( const TemporaryCapturePath& ) = delete;
                TemporaryCapturePath&
                operator=( const TemporaryCapturePath& ) = delete;

                TemporaryCapturePath( TemporaryCapturePath&& other ) noexcept :
                    staging_path_( std::move( other.staging_path_ ) ),
                    capture_path_( std::move( other.capture_path_ ) ),
                    descriptor_( std::exchange( other.descriptor_,
                                                invalidDescriptor ) ),
                    active_( std::exchange( other.active_,
                                            false ) )
                {
                }

                TemporaryCapturePath&
                operator=( TemporaryCapturePath&& ) = delete;

                [[nodiscard]]
                const std::filesystem::path&
                path() const noexcept
                {
                    return capture_path_;
                }

                [[nodiscard]]
                grab::Result<void>
                publish( const std::filesystem::path& destination )
                {
                    auto staged = validate_staged_file( staging_path_, descriptor_ );
                    if( !staged.has_value() )
                    {
                        return staged;
                    }

                    auto publishable = validate_publish_destination( destination );
                    if( !publishable.has_value() )
                    {
                        return publishable;
                    }
                    std::error_code error;
                    std::filesystem::rename( staging_path_, destination, error );
                    if( error )
                    {
                        return filesystem_error( "publish watch capture",
                                                 destination,
                                                 error );
                    }
                    active_ = false;
                    return {};
                }

            private:

                TemporaryCapturePath( std::filesystem::path path,
                                      int                   descriptor ) :
                    staging_path_( std::move( path ) ),
                    capture_path_( std::string{ descriptorPathPrefix } +
                                   std::to_string( descriptor ) ),
                    descriptor_( descriptor )
                {
                }

                void
                discard() noexcept
                {
                    if( !active_ )
                    {
                        return;
                    }
                    remove_staged_file( staging_path_, descriptor_ );
                    active_ = false;
                }

                void
                close_descriptor() noexcept
                {
                    if( descriptor_ == invalidDescriptor )
                    {
                        return;
                    }
                    const int close_result = ::close( descriptor_ );
                    static_cast<void>( close_result );
                    descriptor_ = invalidDescriptor;
                }

                std::filesystem::path staging_path_;
                std::filesystem::path capture_path_;
                int                   descriptor_ = invalidDescriptor;
                bool                  active_{ true };
        };

        [[nodiscard]]
        grab::Result<void>
        write_binary_file( const std::filesystem::path& path,
                           std::span<const std::byte>   bytes )
        {
            if( bytes.size() >
                static_cast<std::size_t>( std::numeric_limits<std::streamsize>::max() ) )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "PNG output is too large: " + path.string() );
            }

            std::vector<char> output;
            output.reserve( bytes.size() );
            for( const std::byte byte : bytes )
            {
                output.push_back(
                    static_cast<char>( std::to_integer<unsigned char>( byte ) )
                );
            }

            std::ofstream stream{ path, std::ios::binary | std::ios::trunc };
            if( !stream )
            {
                return grab::fail( grab::ErrorCode::ProviderFailed,
                                   "failed to open watch capture: " + path.string() );
            }
            if( !output.empty() )
            {
                stream.write( output.data(),
                              static_cast<std::streamsize>( output.size() ) );
            }
            if( !stream )
            {
                return grab::fail( grab::ErrorCode::ProviderFailed,
                                   "failed to write watch capture: " + path.string() );
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        capture_display_to( grab::Screen&                screen,
                            const std::filesystem::path& path )
        {
            auto image = screen.display();
            if( !image.has_value() )
            {
                return std::unexpected( std::move( image.error() ) );
            }
            auto encoded = grab::codec::encode_png( *image );
            if( !encoded.has_value() )
            {
                return std::unexpected( std::move( encoded.error() ) );
            }
            return write_binary_file( path, *encoded );
        }

        [[nodiscard]]
        grab::Result<std::uint32_t>
        count_owned_files( const std::filesystem::path& directory,
                           std::string_view             pattern )
        {
            std::uint32_t   count{};
            std::error_code error;
            constexpr auto  options =
                std::filesystem::directory_options::skip_permission_denied;
            std::filesystem::recursive_directory_iterator iterator{
                directory,
                options,
                error,
            };
            if( error )
            {
                return filesystem_error( "scan watch output", directory, error );
            }

            const std::filesystem::recursive_directory_iterator end;
            while( iterator != end )
            {
                const std::filesystem::path path   = iterator->path();
                const auto                  status = iterator->symlink_status( error );
                if( error )
                {
                    return filesystem_error( "inspect watch output", path, error );
                }
                if( std::filesystem::is_symlink( status ) )
                {
                    iterator.disable_recursion_pending();
                }
                else if( std::filesystem::is_regular_file( status ) )
                {
                    const auto relative = path.lexically_relative( directory );
                    if( grab::config::matches_pattern( pattern,
                                                       relative.generic_string() ) &&
                        count != std::numeric_limits<std::uint32_t>::max() )
                    {
                        ++count;
                    }
                }

                iterator.increment( error );
                if( error )
                {
                    return filesystem_error( "scan watch output", directory, error );
                }
            }
            return count;
        }

        [[nodiscard]]
        std::string
        capture_timestamp( std::chrono::system_clock::time_point now )
        {
            auto rendered = grab::config::render_filename(
                statusTimestampPattern,
                grab::config::PatternContext{ .now = now, .seq = {} }
            );
            if( !rendered.has_value() || rendered->size() < pngExtensionLength )
            {
                return {};
            }
            rendered->resize( rendered->size() - pngExtensionLength );
            return std::move( *rendered );
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
        capture_notification( const std::filesystem::path& path,
                              std::uint32_t                timeout_ms )
        {
            return {
                .app_name   = std::string{ notificationApp },
                .summary    = std::string{ captureSummary },
                .body       = path.string(),
                .icon       = {},
                .timeout_ms = popup_timeout( timeout_ms ),
            };
        }

        [[nodiscard]]
        grab::notify::Notification
        pause_notification( std::uint32_t timeout_ms )
        {
            return {
                .app_name   = std::string{ notificationApp },
                .summary    = std::string{ pauseSummary },
                .body       = std::string{ pauseBody },
                .icon       = {},
                .timeout_ms = popup_timeout( timeout_ms ),
            };
        }

        void
        log_failure( std::string_view tag,
                     std::string_view message )
        {
            grab::log::nominal(
                [tag, message]( grab::log::Event& event )
                {
                    event.tag( tag ).value( "reason", message );
                }
            );
        }

        [[nodiscard]]
        bool
        target_was_lost( grab::ErrorCode code ) noexcept
        {
            return code ==
                   grab::ErrorCode::StaleWindow ||
                   code ==
                   grab::ErrorCode::WindowNotFound ||
                   code == grab::ErrorCode::TargetDetached;
        }

        [[nodiscard]]
        bool
        target_matches_window( const grab::screen::WindowInfo&  window,
                               const grab::config::TargetMatch& match,
                               const std::vector<std::string>&  wm_class_candidates )
        {
            switch( match.kind )
            {
                case grab::config::MatchKind::Pid :
                    return window.pid.has_value() && *window.pid == match.pid;
                case grab::config::MatchKind::WmClass :
                    return grab::screen::wm_class_matches_any( window.wm_class,
                                                               wm_class_candidates );
                case grab::config::MatchKind::Title :
                    return window.title.contains( match.text );
                case grab::config::MatchKind::WindowId :
                    return window.id == match.window_id;
                case grab::config::MatchKind::Count :
                    return false;
            }
            return false;
        }

        [[nodiscard]]
        grab::Result<std::size_t>
        target_match_count( const grab::config::TargetMatch& match )
        {
            if( match.kind == grab::config::MatchKind::Count )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "target match kind is invalid" );
            }
            auto windows = grab::screen::list_windows();
            if( !windows.has_value() )
            {
                return std::unexpected( std::move( windows.error() ) );
            }

            std::vector<std::string> wm_class_candidates;
            if( match.kind == grab::config::MatchKind::WmClass )
            {
                wm_class_candidates = grab::screen::normalized_wm_class_candidates(
                    std::vector<std::string>{ match.text }
                );
            }

            std::size_t count{};
            for( const grab::screen::WindowInfo& window : *windows )
            {
                if( target_matches_window( window, match, wm_class_candidates ) )
                {
                    ++count;
                }
            }
            return count;
        }

    }    // namespace

    class ConfigWatcher::Impl
    {
        public:

            using SteadyClock = std::chrono::steady_clock;
            using TimePoint   = SteadyClock::time_point;

            Impl( grab::config::WatchSection                 watch,
                  std::optional<grab::config::ScriptSection> script,
                  grab::config::NotifySection                notifications,
                  int                                        timer_descriptor,
                  int                                        stop_descriptor ) :
                watch_( std::move( watch ) ),
                script_( std::move( script ) ),
                notifications_( notifications ),
                schedule_(
                    std::chrono::milliseconds{
                        static_cast<std::chrono::milliseconds::rep>( watch_.interval_ms )
                    },
                    script_.has_value() ? std::addressof( *script_ ) : nullptr
                ),
                ledger_( watch_.output,
                         watch_.filename,
                         watch_.limits ),
                timer_descriptor_( timer_descriptor ),
                stop_descriptor_( stop_descriptor )
            {
            }

            ~Impl() noexcept
            {
                stop();
                close_descriptors();
            }

            Impl( const Impl& ) = delete;
            Impl&
            operator=( const Impl& ) = delete;
            Impl( Impl&& )           = delete;
            Impl&
            operator=( Impl&& ) = delete;

            [[nodiscard]]
            grab::Result<void>
            initialize_output();

            [[nodiscard]]
            grab::Result<void>
            initialize_display( const grab::config::DisplaySection& display );

            [[nodiscard]]
            grab::Result<void>
            initialize_target();

            [[nodiscard]]
            grab::Result<void>
            initialize_input();

            void
            initialize_notifier();

            void
            announce_initial_pause();

            void
            start_worker();

            void
            stop() noexcept;

            [[nodiscard]]
            WatchStats
            stats() const;

        private:

            [[nodiscard]]
            grab::Result<void>
            run();

            void
            run_guarded() noexcept;

            [[nodiscard]]
            grab::Result<bool>
            wait_until( TimePoint deadline );

            [[nodiscard]]
            grab::Result<void>
            arm_timer( TimePoint deadline ) const;

            [[nodiscard]]
            grab::Result<void>
            consume_timer() const;

            void
            handle_capture();

            void
            handle_step( std::size_t step_index );

            [[nodiscard]]
            grab::Result<void>
            execute_step( const grab::config::ScriptStep& step );

            [[nodiscard]]
            grab::Result<std::filesystem::path>
            capture_once( std::chrono::system_clock::time_point now );

            [[nodiscard]]
            grab::Result<void>
            capture_target_to( const std::filesystem::path& path );

            [[nodiscard]]
            grab::Result<void>
            resolve_target_now();

            void
            perform_maintenance( TimePoint now );

            [[nodiscard]]
            TimePoint
            next_maintenance() const noexcept;

            void
            synchronize_rotation_state( TimePoint now );

            void
            announce_pause();

            void
            notify_nonfatal( const grab::notify::Notification& notification );

            void
            notification_failure( const grab::Error& error );

            void
            record_error( std::string_view   tag,
                          const grab::Error& error );

            void
            count_error();

            void
            record_capture( std::chrono::system_clock::time_point completed_at );

            void
            record_script_failure( const grab::Error& error );

            void
            update_schedule_stats();

            void
            set_paused_stat( bool paused );

            void
            request_stop() noexcept;

            void
                                                        close_descriptors() noexcept;

            grab::config::WatchSection                  watch_;
            std::optional<grab::config::ScriptSection>  script_;
            grab::config::NotifySection                 notifications_;
            std::string                                 display_name_;
            grab::config::WatchSchedule                 schedule_;
            grab::config::RotationLedger                ledger_;
            std::optional<grab::screen::VirtualDisplay> virtual_display_;
            std::optional<grab::Screen>                 screen_;
            std::optional<grab::Input>                  input_;
            std::optional<grab::notify::Notifier>       notifier_;
            std::optional<std::uint32_t>                target_window_;
            std::uint32_t                               sequence_{};
            int                                         timer_descriptor_;
            int                                         stop_descriptor_;
            std::thread                                 worker_;
            std::atomic_bool                            stop_requested_{ false };
            mutable std::mutex                          stats_mutex_;
            std::mutex                                  stop_mutex_;
            WatchStats                                  stats_;
            TimePoint                                   next_age_prune_;
            TimePoint                                   next_disk_refresh_;
            bool                                        rotation_paused_{};
            bool                                        pause_announced_{};
            bool                                        target_missing_logged_{};
            bool                                        target_resolved_logged_{};
            bool                                        multiple_target_logged_{};
            bool                                        notification_failure_logged_{};
    };

    grab::Result<void>
    ConfigWatcher::Impl::initialize_output()
    {
        auto valid_filename =
            grab::config::render_filename( watch_.filename,
                                           grab::config::PatternContext{
                                               .now = std::chrono::system_clock::now(),
                                               .seq = {},
                                           } );
        if( !valid_filename.has_value() )
        {
            return std::unexpected( std::move( valid_filename.error() ) );
        }
        auto created = ensure_real_directory_chain( watch_.output );
        if( !created.has_value() )
        {
            return created;
        }
        auto scanned = ledger_.scan();
        if( !scanned.has_value() )
        {
            return std::unexpected( std::move( scanned.error() ) );
        }
        auto pruned = ledger_.prune_age( std::chrono::system_clock::now() );
        if( !pruned.has_value() )
        {
            return std::unexpected( std::move( pruned.error() ) );
        }
        auto count = count_owned_files( watch_.output, watch_.filename );
        if( !count.has_value() )
        {
            return std::unexpected( std::move( count.error() ) );
        }

        sequence_           = *count;
        const TimePoint now = SteadyClock::now();
        next_age_prune_     = now + agePruneInterval;
        rotation_paused_    = ledger_.paused();
        next_disk_refresh_  = now + diskRefreshInterval;
        set_paused_stat( rotation_paused_ );
        return {};
    }

    grab::Result<void>
    ConfigWatcher::Impl::initialize_display(
        const grab::config::DisplaySection& display
    )
    {
        if( display.backend == grab::config::DisplayBackend::Count )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "watch display backend is invalid" );
        }
        if( display.backend == grab::config::DisplayBackend::Xvfb )
        {
            auto started = grab::screen::VirtualDisplay::start( display.width,
                                                                display.height,
                                                                display.depth );
            if( !started.has_value() )
            {
                return std::unexpected( std::move( started.error() ) );
            }
            display_name_ = started->display();
            virtual_display_.emplace( std::move( *started ) );
        }
        else if( const auto ambient = ambient_display_name(); ambient.has_value() )
        {
            display_name_ = *ambient;
        }

        grab::Result<grab::Screen> opened =
            display_name_.empty() ? grab::Screen::open()
                                  : with_ambient_display( display_name_,
                                                          [this]
                                                          {
                                                              return grab::Screen::open(
                                                                  display_name_.c_str()
                                                              );
                                                          } );
        if( !opened.has_value() )
        {
            return std::unexpected( std::move( opened.error() ) );
        }
        screen_.emplace( std::move( *opened ) );
        return {};
    }

    grab::Result<void>
    ConfigWatcher::Impl::resolve_target_now()
    {
        if( !watch_.target.has_value() )
        {
            target_window_.reset();
            return {};
        }
        auto resolved = with_ambient_display(
            display_name_,
            [this]
            {
                return grab::screen::resolve_target( *screen_, *watch_.target );
            }
        );
        if( !resolved.has_value() )
        {
            target_window_.reset();
            if( resolved.error().code ==
                grab::ErrorCode::WindowNotFound &&
                !target_missing_logged_ )
            {
                target_missing_logged_ = true;
                log_failure( "config_watch.target_missing", resolved.error().message );
            }
            return std::unexpected( std::move( resolved.error() ) );
        }

        target_window_ = *resolved;
        if( !multiple_target_logged_ )
        {
            auto match_count =
                with_ambient_display( display_name_,
                                      [this]
                                      {
                                          return target_match_count( *watch_.target );
                                      } );
            if( match_count.has_value() && *match_count > 1U )
            {
                multiple_target_logged_ = true;
                grab::log::nominal(
                    [this, count = *match_count]( grab::log::Event& event )
                    {
                        event.tag( "config_watch.multiple_targets" )
                            .value( "match_count", count )
                            .value( "selected_window_id", *target_window_ );
                    }
                );
            }
        }
        if( !target_resolved_logged_ )
        {
            target_resolved_logged_ = true;
            grab::log::nominal(
                [this]( grab::log::Event& event )
                {
                    event.tag( "config_watch.target_resolved" )
                        .value( "window_id", *target_window_ )
                        .value( "selection", "first_match" );
                }
            );
        }
        return {};
    }

    grab::Result<void>
    ConfigWatcher::Impl::initialize_target()
    {
        if( !watch_.target.has_value() )
        {
            return {};
        }
        auto resolved = resolve_target_now();
        if( !resolved.has_value() &&
            resolved.error().code != grab::ErrorCode::WindowNotFound )
        {
            return resolved;
        }
        return {};
    }

    grab::Result<void>
    ConfigWatcher::Impl::initialize_input()
    {
        if( !script_.has_value() || script_->steps.empty() )
        {
            return {};
        }
        const bool needs_input =
            std::ranges::any_of( script_->steps,
                                 []( const grab::config::ScriptStep& step )
                                 {
                                     return step.action !=
                                            grab::config::StepAction::Delay;
                                 } );
        if( !needs_input )
        {
            return {};
        }
        grab::Result<grab::Input> opened =
            display_name_.empty() ? grab::Input::open()
                                  : with_ambient_display( display_name_,
                                                          [this]
                                                          {
                                                              return grab::Input::open(
                                                                  display_name_.c_str()
                                                              );
                                                          } );
        if( !opened.has_value() )
        {
            return std::unexpected( std::move( opened.error() ) );
        }
        input_.emplace( std::move( *opened ) );
        return {};
    }

    void
    ConfigWatcher::Impl::initialize_notifier()
    {
        if( !notifications_.enabled ||
            notifications_.strategy != grab::config::NotifyStrategy::Os )
        {
            return;
        }
        auto opened = grab::notify::Notifier::open();
        if( !opened.has_value() )
        {
            notification_failure( opened.error() );
            return;
        }
        notifier_.emplace( std::move( *opened ) );
    }

    void
    ConfigWatcher::Impl::announce_initial_pause()
    {
        if( rotation_paused_ )
        {
            announce_pause();
        }
    }

    void
    ConfigWatcher::Impl::start_worker()
    {
        worker_ = std::thread(
            [this]
            {
                run_guarded();
            }
        );
    }

    void
    ConfigWatcher::Impl::run_guarded() noexcept
    {
        try
        {
            auto result = run();
            if( !result.has_value() &&
                !stop_requested_.load( std::memory_order_acquire ) )
            {
                record_error( "config_watch.engine_error", result.error() );
            }
        }
        catch( const std::exception& exception )
        {
            const grab::Error error{
                .code    = grab::ErrorCode::InternalFault,
                .message = std::string{ "watch worker exception: " } + exception.what(),
                .capability = {},
                .target     = {},
                .attempts   = {},
            };
            record_error( "config_watch.engine_exception", error );
        }
        catch( ... )
        {
            const grab::Error error{
                .code       = grab::ErrorCode::InternalFault,
                .message    = "watch worker raised an unknown exception",
                .capability = {},
                .target     = {},
                .attempts   = {},
            };
            record_error( "config_watch.engine_exception", error );
        }
    }

    grab::Result<void>
    ConfigWatcher::Impl::run()
    {
        while( !stop_requested_.load( std::memory_order_acquire ) )
        {
            const TimePoint         now = SteadyClock::now();
            const grab::config::Due due = schedule_.next( now );
            update_schedule_stats();
            const TimePoint maintenance = next_maintenance();
            auto            waited = wait_until( std::min( due.wake_at, maintenance ) );
            if( !waited.has_value() )
            {
                return std::unexpected( std::move( waited.error() ) );
            }
            if( !*waited || stop_requested_.load( std::memory_order_acquire ) )
            {
                return {};
            }

            const TimePoint ready_at = SteadyClock::now();
            if( due.wake_at <= ready_at && due.wake_at <= maintenance )
            {
                switch( due.kind )
                {
                    case grab::config::DueKind::Capture :
                        handle_capture();
                        break;
                    case grab::config::DueKind::Step :
                        handle_step( due.step_index );
                        break;
                    case grab::config::DueKind::Idle :
                    case grab::config::DueKind::Count :
                        return grab::fail(
                            grab::ErrorCode::InternalFault,
                            "watch scheduler returned an invalid due kind"
                        );
                }
                continue;
            }
            perform_maintenance( ready_at );
        }
        return {};
    }

    grab::Result<void>
    ConfigWatcher::Impl::arm_timer( TimePoint deadline ) const
    {
        const TimePoint now = SteadyClock::now();
        if( deadline <= now )
        {
            return {};
        }
        const auto remaining =
            std::chrono::ceil<std::chrono::nanoseconds>( deadline - now );
        const auto seconds =
            std::chrono::duration_cast<std::chrono::seconds>( remaining );
        const auto nanoseconds = remaining - seconds;

        if( set_relative_timer( &::timerfd_settime,
                                timer_descriptor_,
                                relativeTimerFlags,
                                seconds.count(),
                                nanoseconds.count() ) == posixFailure )
        {
            return posix_error( "timerfd_settime", errno );
        }
        return {};
    }

    grab::Result<void>
    ConfigWatcher::Impl::consume_timer() const
    {
        std::uint64_t expirations{};
        for( ;; )
        {
            const auto bytes = ::read( timer_descriptor_,
                                       std::addressof( expirations ),
                                       sizeof( expirations ) );
            if( bytes != posixFailure )
            {
                return {};
            }
            if( errno == EINTR )
            {
                continue;
            }
            if( errno == EAGAIN )
            {
                return {};
            }
            return posix_error( "read timerfd", errno );
        }
    }

    grab::Result<bool>
    ConfigWatcher::Impl::wait_until( TimePoint deadline )
    {
        while( !stop_requested_.load( std::memory_order_acquire ) )
        {
            if( deadline <= SteadyClock::now() )
            {
                return true;
            }
            auto armed = arm_timer( deadline );
            if( !armed.has_value() )
            {
                return std::unexpected( std::move( armed.error() ) );
            }

            using PollEvent = decltype( pollfd{}.events );
            std::array<pollfd, pollDescriptorCount> descriptors{
                pollfd{
                       .fd      = timer_descriptor_,
                       .events  = static_cast<PollEvent>( POLLIN ),
                       .revents = static_cast<PollEvent>( noEvents ),
                       },
                pollfd{
                       .fd      = stop_descriptor_,
                       .events  = static_cast<PollEvent>( POLLIN ),
                       .revents = static_cast<PollEvent>( noEvents ),
                       },
            };
            const int result = ::poll( descriptors.data(),
                                       static_cast<nfds_t>( descriptors.size() ),
                                       infinitePollTimeout );
            if( result == posixFailure )
            {
                if( errno == EINTR )
                {
                    continue;
                }
                return posix_error( "poll watch timer", errno );
            }
            if( ( descriptors.at( stopDescriptorIndex ).revents & POLLIN ) != noEvents )
            {
                return false;
            }
            if( ( descriptors.at( timerDescriptorIndex ).revents & POLLIN ) != noEvents )
            {
                auto consumed = consume_timer();
                if( !consumed.has_value() )
                {
                    return std::unexpected( std::move( consumed.error() ) );
                }
                return true;
            }
            return grab::fail( grab::ErrorCode::InternalFault,
                               "watch timer poll returned no readable descriptor" );
        }
        return false;
    }

    grab::Result<void>
    ConfigWatcher::Impl::capture_target_to( const std::filesystem::path& path )
    {
        if( !target_window_.has_value() )
        {
            auto resolved = resolve_target_now();
            if( !resolved.has_value() )
            {
                return resolved;
            }
        }

        auto captured = with_ambient_display(
            display_name_,
            [this, &path]
            {
                return grab::screen::capture_window_to( *screen_,
                                                        *target_window_,
                                                        path.string() );
            }
        );
        if( captured.has_value() || !target_was_lost( captured.error().code ) )
        {
            return captured;
        }

        target_window_.reset();
        auto resolved = resolve_target_now();
        if( !resolved.has_value() )
        {
            return resolved;
        }
        captured = with_ambient_display(
            display_name_,
            [this, &path]
            {
                return grab::screen::capture_window_to( *screen_,
                                                        *target_window_,
                                                        path.string() );
            }
        );
        if( !captured.has_value() && target_was_lost( captured.error().code ) )
        {
            target_window_.reset();
        }
        return captured;
    }

    grab::Result<std::filesystem::path>
    ConfigWatcher::Impl::capture_once( std::chrono::system_clock::time_point now )
    {
        auto rendered = grab::config::render_filename(
            watch_.filename,
            grab::config::PatternContext{ .now = now, .seq = sequence_ }
        );
        if( !rendered.has_value() )
        {
            return std::unexpected( std::move( rendered.error() ) );
        }
        std::filesystem::path path = ( watch_.output / *rendered ).lexically_normal();
        auto parent = validate_capture_directory( watch_.output, path.parent_path() );
        if( !parent.has_value() )
        {
            return std::unexpected( std::move( parent.error() ) );
        }
        auto staged = TemporaryCapturePath::create( path.parent_path() );
        if( !staged.has_value() )
        {
            return std::unexpected( std::move( staged.error() ) );
        }

        if( !screen_.has_value() )
        {
            return grab::fail( grab::ErrorCode::InternalFault,
                               "watch screen connection is missing" );
        }
        grab::Result<void> captured = watch_.target.has_value()
                                        ? capture_target_to( staged->path() )
                                        : capture_display_to( *screen_, staged->path() );
        if( !captured.has_value() )
        {
            return std::unexpected( std::move( captured.error() ) );
        }
        auto published = staged->publish( path );
        if( !published.has_value() )
        {
            return std::unexpected( std::move( published.error() ) );
        }
        auto adopted = ledger_.adopt( path );
        if( !adopted.has_value() )
        {
            return std::unexpected( std::move( adopted.error() ) );
        }
        if( sequence_ != std::numeric_limits<std::uint32_t>::max() )
        {
            ++sequence_;
        }
        return path;
    }

    void
    ConfigWatcher::Impl::handle_capture()
    {
        if( !ledger_.paused() )
        {
            auto captured = capture_once( std::chrono::system_clock::now() );
            if( !captured.has_value() )
            {
                if( watch_.target.has_value() &&
                    target_missing_logged_ &&
                    captured.error().code == grab::ErrorCode::WindowNotFound )
                {
                    count_error();
                }
                else
                {
                    record_error( "config_watch.capture_error", captured.error() );
                }
            }
            else
            {
                record_capture( std::chrono::system_clock::now() );
                notify_nonfatal(
                    capture_notification( *captured, notifications_.popup_timeout_ms )
                );
            }
            synchronize_rotation_state( SteadyClock::now() );
        }
        schedule_.capture_done( SteadyClock::now() );
    }

    grab::Result<void>
    ConfigWatcher::Impl::execute_step( const grab::config::ScriptStep& step )
    {
        if( step.action == grab::config::StepAction::Delay )
        {
            return {};
        }
        if( !input_.has_value() )
        {
            return grab::fail( grab::ErrorCode::InternalFault,
                               "watch script has no input connection" );
        }
        grab::Input& input = *input_;
        switch( step.action )
        {
            case grab::config::StepAction::Move :
                return input.move( step.x, step.y );
            case grab::config::StepAction::Click :
                return input.click( step.button );
            case grab::config::StepAction::ClickAt :
                return input.click_at( step.x, step.y, step.button );
            case grab::config::StepAction::Drag :
                return input.drag( { .x = step.x, .y = step.y },
                                   { .x = step.to_x, .y = step.to_y } );
            case grab::config::StepAction::Type :
                return input.type_text( step.text );
            case grab::config::StepAction::Key :
                return input.press_key( step.text );
            case grab::config::StepAction::Delay :
                return {};
            case grab::config::StepAction::Count :
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "watch script action is invalid" );
        }
        return grab::fail( grab::ErrorCode::InternalFault,
                           "watch script action is unreachable" );
    }

    void
    ConfigWatcher::Impl::handle_step( std::size_t step_index )
    {
        if( !script_.has_value() || step_index >= script_->steps.size() )
        {
            const grab::Error error{
                .code       = grab::ErrorCode::InternalFault,
                .message    = "watch scheduler returned an invalid step index",
                .capability = {},
                .target     = {},
                .attempts   = {},
            };
            schedule_.fail_script();
            static_cast<void>( schedule_.next( SteadyClock::now() ) );
            update_schedule_stats();
            record_script_failure( error );
            return;
        }
        auto executed = execute_step( script_->steps.at( step_index ) );
        if( !executed.has_value() )
        {
            schedule_.fail_script();
            static_cast<void>( schedule_.next( SteadyClock::now() ) );
            update_schedule_stats();
            record_script_failure( executed.error() );
            return;
        }
        const TimePoint completed_at = SteadyClock::now();
        schedule_.step_done( completed_at );
        static_cast<void>( schedule_.next( completed_at ) );
        update_schedule_stats();
    }

    ConfigWatcher::Impl::TimePoint
    ConfigWatcher::Impl::next_maintenance() const noexcept
    {
        if( !rotation_paused_ )
        {
            return next_age_prune_;
        }
        return std::min( next_age_prune_, next_disk_refresh_ );
    }

    void
    ConfigWatcher::Impl::perform_maintenance( TimePoint now )
    {
        if( now >= next_age_prune_ )
        {
            auto pruned     = ledger_.prune_age( std::chrono::system_clock::now() );
            next_age_prune_ = SteadyClock::now() + agePruneInterval;
            if( !pruned.has_value() )
            {
                record_error( "config_watch.age_prune_error", pruned.error() );
            }
            synchronize_rotation_state( SteadyClock::now() );
        }
        if( rotation_paused_ && now >= next_disk_refresh_ )
        {
            auto refreshed     = ledger_.refresh_disk();
            next_disk_refresh_ = SteadyClock::now() + diskRefreshInterval;
            if( !refreshed.has_value() )
            {
                record_error( "config_watch.disk_refresh_error", refreshed.error() );
            }
            synchronize_rotation_state( SteadyClock::now() );
        }
    }

    void
    ConfigWatcher::Impl::synchronize_rotation_state( TimePoint now )
    {
        const bool paused = ledger_.paused();
        set_paused_stat( paused );
        if( paused && !rotation_paused_ )
        {
            rotation_paused_   = true;
            next_disk_refresh_ = now + diskRefreshInterval;
            announce_pause();
            return;
        }
        if( !paused && rotation_paused_ )
        {
            rotation_paused_ = false;
            pause_announced_ = false;
            grab::log::nominal(
                []( grab::log::Event& event )
                {
                    event.tag( "config_watch.rotation_resumed" );
                }
            );
        }
    }

    void
    ConfigWatcher::Impl::announce_pause()
    {
        if( pause_announced_ )
        {
            return;
        }
        pause_announced_ = true;
        grab::log::nominal(
            []( grab::log::Event& event )
            {
                event.tag( "config_watch.rotation_paused" );
            }
        );
        notify_nonfatal( pause_notification( notifications_.popup_timeout_ms ) );
    }

    void
    ConfigWatcher::Impl::notify_nonfatal(
        const grab::notify::Notification& notification
    )
    {
        if( !notifier_.has_value() )
        {
            return;
        }
        auto notified = notifier_->notify( notification );
        if( !notified.has_value() )
        {
            notification_failure( notified.error() );
        }
    }

    void
    ConfigWatcher::Impl::notification_failure( const grab::Error& error )
    {
        notifier_.reset();
        if( notification_failure_logged_ )
        {
            return;
        }
        notification_failure_logged_ = true;
        log_failure( "config_watch.notification_error", error.message );
    }

    void
    ConfigWatcher::Impl::record_error( std::string_view   tag,
                                       const grab::Error& error )
    {
        count_error();
        log_failure( tag, error.message );
    }

    void
    ConfigWatcher::Impl::count_error()
    {
        const std::scoped_lock lock( stats_mutex_ );
        if( stats_.errors != std::numeric_limits<std::uint64_t>::max() )
        {
            ++stats_.errors;
        }
    }

    void
    ConfigWatcher::Impl::record_capture(
        std::chrono::system_clock::time_point completed_at
    )
    {
        std::string            timestamp = capture_timestamp( completed_at );
        const std::scoped_lock lock( stats_mutex_ );
        if( stats_.captured != std::numeric_limits<std::uint64_t>::max() )
        {
            ++stats_.captured;
        }
        stats_.last_capture = std::move( timestamp );
    }

    void
    ConfigWatcher::Impl::record_script_failure( const grab::Error& error )
    {
        {
            const std::scoped_lock lock( stats_mutex_ );
            stats_.script_failed = true;
        }
        log_failure( "config_watch.script_error", error.message );
    }

    void
    ConfigWatcher::Impl::update_schedule_stats()
    {
        const std::scoped_lock lock( stats_mutex_ );
        stats_.skipped = schedule_.skipped_captures();
    }

    void
    ConfigWatcher::Impl::set_paused_stat( bool paused )
    {
        const std::scoped_lock lock( stats_mutex_ );
        stats_.paused = paused;
    }

    WatchStats
    ConfigWatcher::Impl::stats() const
    {
        const std::scoped_lock lock( stats_mutex_ );
        return stats_;
    }

    void
    ConfigWatcher::Impl::request_stop() noexcept
    {
        stop_requested_.store( true, std::memory_order_release );
        for( ;; )
        {
            const auto bytes = ::write( stop_descriptor_,
                                        std::addressof( wakeEventValue ),
                                        sizeof( wakeEventValue ) );
            if( bytes != posixFailure )
            {
                return;
            }
            if( errno == EINTR )
            {
                continue;
            }
            return;
        }
    }

    void
    ConfigWatcher::Impl::stop() noexcept
    {
        const std::scoped_lock lock( stop_mutex_ );
        request_stop();
        if( worker_.joinable() )
        {
            worker_.join();
        }
    }

    void
    ConfigWatcher::Impl::close_descriptors() noexcept
    {
        if( timer_descriptor_ != invalidDescriptor )
        {
            const int close_result = ::close( timer_descriptor_ );
            static_cast<void>( close_result );
            timer_descriptor_ = invalidDescriptor;
        }
        if( stop_descriptor_ != invalidDescriptor )
        {
            const int close_result = ::close( stop_descriptor_ );
            static_cast<void>( close_result );
            stop_descriptor_ = invalidDescriptor;
        }
    }

    ConfigWatcher::ConfigWatcher( std::unique_ptr<Impl> impl ) noexcept :
        impl_( std::move( impl ) )
    {
    }

    ConfigWatcher::~ConfigWatcher()
    {
        stop();
    }

    ConfigWatcher::ConfigWatcher( ConfigWatcher&& other ) noexcept = default;

    ConfigWatcher&
    ConfigWatcher::operator=( ConfigWatcher&& other ) noexcept
    {
        if( this != &other )
        {
            stop();
            impl_ = std::move( other.impl_ );
        }
        return *this;
    }

    grab::Result<ConfigWatcher>
    ConfigWatcher::start( const grab::config::Config& cfg )
    {
        try
        {
            if( !cfg.watch.has_value() )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "config does not contain a watch section" );
            }
            if( cfg.watch->interval_ms < minimumWatchIntervalMs )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "watch interval must be at least 20 ms" );
            }
            if( cfg.watch->output.empty() || cfg.watch->format != pngFormat )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "watch output and PNG format are required" );
            }

            grab::config::WatchSection watch = *cfg.watch;
            auto                       validated_filename =
                grab::config::render_filename( watch.filename,
                                               grab::config::PatternContext{
                                                   .now =
                                                       std::chrono::system_clock::now(),
                                                   .seq = {},
                                               } );
            if( !validated_filename.has_value() )
            {
                return std::unexpected( std::move( validated_filename.error() ) );
            }
            std::string effective_pattern = watch.filename;
            if( !effective_pattern.ends_with( pngExtension ) )
            {
                effective_pattern.append( pngExtension );
            }
            watch.filename = std::filesystem::path{ effective_pattern }
                                 .lexically_normal()
                                 .generic_string();
            std::error_code path_error;
            watch.output =
                std::filesystem::absolute( watch.output, path_error ).lexically_normal();
            if( path_error )
            {
                return filesystem_error( "resolve watch output",
                                         cfg.watch->output,
                                         path_error );
            }

            ScopedDescriptor timer{ ::timerfd_create( linuxMonotonicClockId,
                                                      TFD_CLOEXEC | TFD_NONBLOCK ) };
            if( timer.get() == invalidDescriptor )
            {
                return posix_error( "timerfd_create", errno );
            }
            ScopedDescriptor stop_event{ ::eventfd( 0U, EFD_CLOEXEC | EFD_NONBLOCK ) };
            if( stop_event.get() == invalidDescriptor )
            {
                return posix_error( "eventfd", errno );
            }

            auto impl = std::make_unique<Impl>( std::move( watch ),
                                                cfg.script,
                                                cfg.notifications,
                                                timer.get(),
                                                stop_event.get() );
            static_cast<void>( timer.release() );
            static_cast<void>( stop_event.release() );
            auto initialized = impl->initialize_output();
            if( !initialized.has_value() )
            {
                return std::unexpected( std::move( initialized.error() ) );
            }
            initialized = impl->initialize_display( cfg.display );
            if( !initialized.has_value() )
            {
                return std::unexpected( std::move( initialized.error() ) );
            }
            initialized = impl->initialize_target();
            if( !initialized.has_value() )
            {
                return std::unexpected( std::move( initialized.error() ) );
            }
            initialized = impl->initialize_input();
            if( !initialized.has_value() )
            {
                return std::unexpected( std::move( initialized.error() ) );
            }
            impl->initialize_notifier();
            impl->announce_initial_pause();
            impl->start_worker();
            return ConfigWatcher{ std::move( impl ) };
        }
        catch( const std::exception& exception )
        {
            return grab::fail( grab::ErrorCode::InternalFault,
                               std::string{ "failed to start config watcher: " } +
                                   exception.what() );
        }
        catch( ... )
        {
            return grab::fail( grab::ErrorCode::InternalFault,
                               "failed to start config watcher" );
        }
    }

    void
    ConfigWatcher::stop()
    {
        if( impl_ != nullptr )
        {
            impl_->stop();
        }
    }

    WatchStats
    ConfigWatcher::stats() const
    {
        return impl_ == nullptr ? WatchStats{} : impl_->stats();
    }

}    // namespace grab::screen
