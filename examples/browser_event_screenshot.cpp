// browser_event_screenshot — capture a full-display screenshot on every
// browser event and save it as a timestamped PNG.
//
//   ./browser_event_screenshot [output-dir] [--socket <path>]
//
// Connects a browser through the same native-messaging unix socket as the
// event_logger example (see browser_socket.hpp for the manifest/socat
// wiring). On each browser event (tab switch, context/URL update) it captures
// the whole display via grab::Screen and writes
// <output-dir>/<YYYYMMDD-HHMMSS-mmm>-<kind>.png. Runs until Ctrl+C.

#include "browser_socket.hpp"
#include "codec/png.hpp"
#include "grab/event.hpp"
#include "grab/result.hpp"
#include "grab/screen.hpp"
#include "grab/session.hpp"
#include "grab/watch.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

namespace
{

    constexpr std::size_t queueCapacity = 1'024U;
    constexpr int         signalSuccess = 0;
    // POSIX timespec::tv_nsec is specified as long.
    // NOLINTNEXTLINE(google-runtime-int)
    constexpr long        pollTickNanos      = 300'000'000L;    // 300 ms
    constexpr double      millisPerSecond    = 1'000.0;
    constexpr std::size_t stampBufferSize    = 24U;
    constexpr int         maxCollisionSuffix = 1'000;
    constexpr int         tmYearBase         = 1'900;
    constexpr int         tmMonthBase        = 1;

    // Short filename label per browser event kind.
    [[nodiscard]]
    std::string_view
    kind_label( grab::EventKind kind )
    {
        switch( kind )
        {
            case grab::EventKind::AppTabChanged :
                return "tab_changed";
            case grab::EventKind::AppContextUpdate :
                return "context_update";
            default :
                return "browser_event";
        }
    }

    constexpr std::string_view tabTitleKey = "tab_title";

    // The bridge fills IntegrationEvent::title only from the frame's "title";
    // extension frames that use "tab_title" leave it empty, so fall back to
    // the raw json (mirrors the event_logger example).
    [[nodiscard]]
    std::string
    integration_title( const grab::IntegrationEvent& event )
    {
        if( !event.title.empty() )
        {
            return event.title;
        }
        const auto parsed =
            // NOLINTNEXTLINE(misc-include-cleaner): provided by nlohmann/json.hpp.
            nlohmann::json::parse( event.json, nullptr, /*allow_exceptions=*/false );
        if( parsed.is_object() )
        {
            if( const auto entry = parsed.find( tabTitleKey );
                entry != parsed.end() && entry->is_string() )
            {
                return entry->get<std::string>();
            }
        }
        return {};
    }

    // Human description for the stdout line, from the integration payload.
    [[nodiscard]]
    std::string
    describe_browser( const grab::Event& event )
    {
        const auto* payload = std::get_if<grab::IntegrationEvent>( &event.payload );
        if( payload == nullptr )
        {
            return std::string{ kind_label( event.kind ) };
        }
        if( event.kind == grab::EventKind::AppTabChanged )
        {
            return "tab \"" + integration_title( *payload ) + "\"";
        }
        return "context \"" + payload->detail + "\"";
    }

    // <YYYYMMDD-HHMMSS-mmm> in local time from an epoch-seconds timestamp.
    [[nodiscard]]
    std::string
    format_stamp( double epoch_s )
    {
        const auto seconds = static_cast<std::time_t>( epoch_s );
        const auto millis =
            static_cast<int>( ( epoch_s - std::floor( epoch_s ) ) * millisPerSecond );
        std::tm local{};
        ::localtime_r( &seconds, &local );
        std::array<char, stampBufferSize> buffer{};
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        static_cast<void>( std::snprintf( buffer.data(),
                                          buffer.size(),
                                          "%04d%02d%02d-%02d%02d%02d-%03d",
                                          local.tm_year + tmYearBase,
                                          local.tm_mon + tmMonthBase,
                                          local.tm_mday,
                                          local.tm_hour,
                                          local.tm_min,
                                          local.tm_sec,
                                          millis ) );
        return std::string{ buffer.data() };
    }

    // <dir>/<stamp>-<kind>.png, with a -N suffix if that path already exists.
    [[nodiscard]]
    std::filesystem::path
    capture_filename( const std::filesystem::path& dir,
                      double                       epoch_s,
                      std::string_view             kind )
    {
        const std::string     stem = format_stamp( epoch_s ) + "-" + std::string{ kind };
        std::filesystem::path candidate = dir / ( stem + ".png" );
        for( int suffix = 1; suffix < maxCollisionSuffix; ++suffix )
        {
            std::error_code exists_error;
            if( !std::filesystem::exists( candidate, exists_error ) )
            {
                break;
            }
            candidate = dir / ( stem + "-" + std::to_string( suffix ) + ".png" );
        }
        return candidate;
    }

    // Owns the Screen and the output directory. Captures the full display and
    // writes a PNG per browser event, on the session reactor thread. Browser
    // events are infrequent, so the synchronous capture stall is acceptable.
    class ScreenshotSaver
    {
        public:

            ScreenshotSaver( grab::Screen          screen,
                             std::filesystem::path dir ) :
                screen_{ std::move( screen ) },
                dir_{ std::move( dir ) }
            {
            }

            void
            capture_and_save( const grab::Event& event )
            {
                if( failed_ )
                {
                    return;
                }
                auto image = screen_.display();
                if( !image.has_value() )
                {
                    fail( std::move( image.error() ) );
                    return;
                }
                auto encoded = grab::codec::encode_png( *image );
                if( !encoded.has_value() )
                {
                    fail( std::move( encoded.error() ) );
                    return;
                }
                const std::filesystem::path path =
                    capture_filename( dir_, event.timestamp, kind_label( event.kind ) );
                std::ofstream stream{ path, std::ios::binary };
                if( !stream )
                {
                    fail( grab::Error{
                        .code       = grab::ErrorCode::InternalFault,
                        .message    = "cannot open " + path.string(),
                        .capability = {},
                        .target     = {},
                        .attempts   = {},
                    } );
                    return;
                }
                stream.write(
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                    reinterpret_cast<const char*>( encoded->data() ),
                    static_cast<std::streamsize>( encoded->size() )
                );
                if( !stream )
                {
                    fail( grab::Error{
                        .code       = grab::ErrorCode::InternalFault,
                        .message    = "write failed: " + path.string(),
                        .capability = {},
                        .target     = {},
                        .attempts   = {},
                    } );
                    return;
                }
                ++saved_;
                std::cout << "saved " << path.string() << " ("
                          << describe_browser( event ) << ")\n";
                std::cout.flush();
            }

            [[nodiscard]]
            std::size_t
            saved() const noexcept
            {
                return saved_;
            }

            [[nodiscard]]
            std::optional<grab::Error>
            error() const
            {
                const std::scoped_lock lock{ error_mutex_ };
                return error_;
            }

        private:

            void
            fail( grab::Error error )
            {
                failed_ = true;
                std::cerr << "browser_event_screenshot: capture stopped: "
                          << error.message << '\n';
                const std::scoped_lock lock{ error_mutex_ };
                if( !error_.has_value() )
                {
                    error_ = std::move( error );
                }
            }

            grab::Screen               screen_;
            std::filesystem::path      dir_;
            std::size_t                saved_  = 0U;
            bool                       failed_ = false;
            mutable std::mutex         error_mutex_;
            std::optional<grab::Error> error_;
    };

    // Notify -> post -> drain on the session reactor, with a tail-draining
    // stop (same discipline as the event_logger / mouse_snake_trail pumps).
    class CapturePump
    {
        public:

            CapturePump( grab::Session&     session,
                         grab::Subscription subscription,
                         ScreenshotSaver&   saver ) :
                session_{ &session },
                subscription_{ std::move( subscription ) },
                saver_{ &saver }
            {
            }

            void
            install()
            {
                subscription_.set_notify(
                    [this]
                    {
                        schedule();
                    }
                );
            }

            [[nodiscard]]
            grab::Result<void>
            stop()
            {
                subscription_.set_notify( {} );
                session_->stop_observation();
                std::promise<void> fence;
                auto               reached = fence.get_future();
                auto               posted  = session_->post(
                    [this, &fence]
                    {
                        drain();
                        fence.set_value();
                    }
                );
                if( !posted.has_value() )
                {
                    return posted;
                }
                reached.get();
                return {};
            }

        private:

            void
            schedule()
            {
                bool expected = false;
                if( !scheduled_.compare_exchange_strong( expected, true ) )
                {
                    return;
                }
                auto posted = session_->post(
                    [this]
                    {
                        drain();
                    }
                );
                if( !posted.has_value() )
                {
                    scheduled_.store( false );
                }
            }

            void
            drain()
            {
                scheduled_.store( false );
                while( auto item = subscription_.try_pop_item() )
                {
                    if( const auto* event = std::get_if<grab::Event>( &*item );
                        event !=
                        nullptr &&
                        event->category == grab::EventCategory::Integration )
                    {
                        saver_->capture_and_save( *event );
                    }
                }
            }

            grab::Session*     session_;
            grab::Subscription subscription_;
            ScreenshotSaver*   saver_;
            std::atomic_bool   scheduled_{ false };
    };

    [[nodiscard]]
    bool
    block_shutdown_signals(
        sigset_t& signals
    ) noexcept    // NOLINT(misc-include-cleaner)
    {
        // NOLINTNEXTLINE(misc-include-cleaner): POSIX <signal.h>.
        if( ::sigemptyset( &signals ) != signalSuccess )
        {
            return false;
        }
        // NOLINTNEXTLINE(misc-include-cleaner): POSIX <signal.h>.
        if( ::sigaddset( &signals, SIGINT ) !=
            signalSuccess ||
            ::sigaddset( &signals, SIGTERM ) != signalSuccess )
        {
            return false;
        }
        // NOLINTNEXTLINE(misc-include-cleaner): POSIX <signal.h>.
        return ::pthread_sigmask( SIG_BLOCK, &signals, nullptr ) == signalSuccess;
    }

    [[nodiscard]]
    bool
    shutdown_requested(
        const sigset_t& signals    // NOLINT(misc-include-cleaner)
    ) noexcept
    {
        const timespec timeout{ .tv_sec = 0, .tv_nsec = pollTickNanos };
        // NOLINTNEXTLINE(misc-include-cleaner): POSIX <signal.h>.
        const int      received = ::sigtimedwait( &signals, nullptr, &timeout );
        return received == SIGINT || received == SIGTERM;
    }

    [[nodiscard]]
    grab::Result<void>
    // NOLINTNEXTLINE(readability-function-size): lifecycle is intentionally linear.
    run( std::span<char*> args )
    {
        std::filesystem::path output_dir{ "browser-screenshots" };
        std::string           socket_path =
            grab::examples::default_socket_path( "grab-browser-shots.sock" );
        for( std::size_t index = 0U; index < args.size(); ++index )
        {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            const std::string_view arg{ args[index] };
            if( arg == "--socket" && index + 1U < args.size() )
            {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                socket_path = args[index + 1U];
                ++index;
            }
            else if( index == 0U )
            {
                output_dir = arg;
            }
        }

        std::error_code dir_error;
        std::filesystem::create_directories( output_dir, dir_error );
        if( dir_error )
        {
            return std::unexpected( grab::Error{
                .code = grab::ErrorCode::InternalFault,
                .message =
                    "cannot create " + output_dir.string() + ": " + dir_error.message(),
                .capability = {},
                .target     = {},
                .attempts   = {},
            } );
        }

        sigset_t signals{};
        // Block before Session::open() so the reactor thread inherits the mask.
        if( !block_shutdown_signals( signals ) )
        {
            return std::unexpected( grab::Error{
                .code       = grab::ErrorCode::InternalFault,
                .message    = "failed to block shutdown signals",
                .capability = {},
                .target     = {},
                .attempts   = {},
            } );
        }

        auto session = grab::Session::open();
        if( !session.has_value() )
        {
            return std::unexpected( std::move( session.error() ) );
        }

        auto screen = grab::Screen::open();
        if( !screen.has_value() )
        {
            return std::unexpected( std::move( screen.error() ) );
        }

        ScreenshotSaver         saver{ std::move( *screen ), output_dir };

        // Empty kinds expands to all registered kinds; the filter narrows
        // delivery to browser (Integration-category) events only.
        grab::SubscriptionScope scope;
        scope.filter.categories = { grab::EventCategory::Integration };
        auto subscription =
            ( *session )
                ->watch( std::move( scope ),
                         grab::QueueOptions{
                             .capacity = queueCapacity,
                             .overflow = grab::QueueOverflowPolicy::NeverDrop,
                         } );
        if( !subscription.has_value() )
        {
            return std::unexpected( std::move( subscription.error() ) );
        }

        CapturePump pump{ **session, std::move( *subscription ), saver };
        pump.install();

        auto observing = ( *session )->start_observation();
        if( !observing.has_value() )
        {
            return std::unexpected( std::move( observing.error() ) );
        }

        auto browser_socket =
            grab::examples::BrowserSocket::open( socket_path, **session );
        if( !browser_socket.has_value() )
        {
            auto stopped = pump.stop();
            static_cast<void>( stopped );
            ( *session )->close();
            return std::unexpected( std::move( browser_socket.error() ) );
        }

        std::cout << "browser_event_screenshot: browser socket at " << socket_path
                  << "\nbrowser_event_screenshot: saving to " << output_dir.string()
                  << " (Ctrl+C to stop)\n";
        std::cout.flush();

        while( !shutdown_requested( signals ) )
        {
        }

        browser_socket->stop( **session );
        auto stopped = pump.stop();
        ( *session )->close();

        std::cout << "browser_event_screenshot: " << saver.saved()
                  << " screenshots saved to " << output_dir.string() << '\n';
        std::cout.flush();

        if( !stopped.has_value() )
        {
            return stopped;
        }
        if( auto error = saver.error(); error.has_value() )
        {
            return std::unexpected( std::move( *error ) );
        }
        return {};
    }

}    // namespace

int
main( int   argc,
      char* argv[] )
{
    auto result =
        run( std::span{ argv, static_cast<std::size_t>( argc ) }.subspan( 1 ) );
    if( !result.has_value() )
    {
        std::cerr << "browser_event_screenshot: " << result.error().message << '\n';
        return 1;
    }
    return 0;
}
