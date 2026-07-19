// event_logger — live terminal feed of everything grab observes.
//
//   ./event_logger [recording-dir] [--socket <path>]
//
// One line per event: "HH:MM:SS.mmm -> <category> event -> <description>".
// Runs until Ctrl+C. Later tasks add mouse-move coalescing, JSONL
// recording, window tracking, and a browser-bridge socket.

#include "grab/event.hpp"
#include "grab/event_descriptor.hpp"
#include "grab/result.hpp"
#include "grab/session.hpp"
#include "grab/watch.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <expected>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace
{

    constexpr std::size_t queueCapacity = 8'192U;
    constexpr int         signalSuccess = 0;
    // POSIX timespec::tv_nsec is specified as long.
    // NOLINTNEXTLINE(google-runtime-int)
    constexpr long        pollTickNanos         = 300'000'000L;    // 300 ms
    constexpr int         labelWidth            = 7;               // "browser"
    constexpr double      millisPerSecond       = 1'000.0;
    constexpr std::size_t timestampBufferSize   = 16U;
    constexpr std::size_t detailBufferSize      = 32U;
    constexpr double      bytesPerKilobyte      = 1'024.0;
    constexpr double      coalesceWindowSeconds = 0.3;

    // ---- formatting -----------------------------------------------------

    [[nodiscard]]
    std::string
    format_timestamp( double epoch_s )
    {
        const auto seconds = static_cast<std::time_t>( epoch_s );
        const auto millis =
            static_cast<int>( ( epoch_s - std::floor( epoch_s ) ) * millisPerSecond );
        std::tm local{};
        ::localtime_r( &seconds, &local );
        std::array<char, timestampBufferSize> buffer{};
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        static_cast<void>( std::snprintf( buffer.data(),
                                          buffer.size(),
                                          "%02d:%02d:%02d.%03d",
                                          local.tm_hour,
                                          local.tm_min,
                                          local.tm_sec,
                                          millis ) );
        return std::string{ buffer.data() };
    }

    [[nodiscard]]
    std::string_view
    category_label( grab::EventKind kind )
    {
        switch( kind )
        {
            case grab::EventKind::NodeAdded :
            case grab::EventKind::NodeRemoved :
            case grab::EventKind::NodeChanged :
            case grab::EventKind::RelationAdded :
            case grab::EventKind::RelationRemoved :
            case grab::EventKind::ActiveChildChanged :
                return "ui";
            default :
                break;
        }
        switch( grab::category_of( kind ) )
        {
            case grab::EventCategory::Input :
                return "input";
            case grab::EventCategory::Window :
                return "os";
            case grab::EventCategory::Accessibility :
                return "a11y";
            case grab::EventCategory::Integration :
                return "browser";
            case grab::EventCategory::State :
                return "state";
            default :
                return "event";
        }
    }

    [[nodiscard]]
    std::string
    feed_line( double             timestamp,
               grab::EventKind    kind,
               const std::string& description )
    {
        std::ostringstream line;
        line << format_timestamp( timestamp ) << " -> ";
        const std::string_view label = category_label( kind );
        line << label;
        for( int pad = static_cast<int>( label.size() ); pad < labelWidth; ++pad )
        {
            line << ' ';
        }
        line << " event -> " << description;
        return std::move( line ).str();
    }

    [[nodiscard]]
    std::string
    quoted_or_number( const std::string& name,
                      std::uint32_t      code,
                      char               fallback_prefix )
    {
        if( !name.empty() )
        {
            return "\"" + name + "\"";
        }
        return std::string{ fallback_prefix } + std::to_string( code );
    }

    [[nodiscard]]
    std::string
    // Exhaustive vocabulary dispatch is intentionally kept in one formatter.
    // NOLINTNEXTLINE(readability-function-size,readability-function-cognitive-complexity)
    describe( const grab::Event& event )
    {
        using Kind = grab::EventKind;
        switch( event.kind )
        {
            case Kind::KeyDown :
            case Kind::KeyUp :
                {
                    const auto* key = std::get_if<grab::InputKey>( &event.payload );
                    if( key == nullptr )
                    {
                        break;
                    }
                    return "key " +
                           quoted_or_number( key->name, key->code, '#' ) +
                           ( event.kind == Kind::KeyDown ? " pressed" : " released" );
                }
            case Kind::KeyCombo :
                {
                    const auto* combo = std::get_if<grab::KeyCombo>( &event.payload );
                    if( combo == nullptr )
                    {
                        break;
                    }
                    return "combo \"" + combo->text + "\" pressed";
                }
            case Kind::MouseClick :
                {
                    const auto* click = std::get_if<grab::MouseClick>( &event.payload );
                    if( click == nullptr )
                    {
                        break;
                    }
                    return "button " +
                           quoted_or_number( click->name, click->button, '#' ) +
                           " clicked";
                }
            case Kind::MouseMove :
                {
                    const auto* move = std::get_if<grab::MouseMove>( &event.payload );
                    if( move == nullptr )
                    {
                        break;
                    }
                    if( move->position.has_value() )
                    {
                        return "pointer moved to (" +
                               std::to_string(
                                   static_cast<std::int64_t>( move->position->x )
                               ) +
                               ", " +
                               std::to_string(
                                   static_cast<std::int64_t>( move->position->y )
                               ) +
                               ")";
                    }
                    return "pointer moved (" + move->axis + ")";
                }
            case Kind::IdleStart :
            case Kind::IdleEnd :
                {
                    const auto* idle = std::get_if<grab::Idle>( &event.payload );
                    if( idle == nullptr )
                    {
                        break;
                    }
                    std::array<char, detailBufferSize> detail{};
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
                    static_cast<void>( std::snprintf( detail.data(),
                                                      detail.size(),
                                                      "%.1fs",
                                                      idle->idle_s ) );
                    return event.kind == Kind::IdleStart
                             ? "idle started (" + std::string{ detail.data() } + ")"
                             : "idle ended (" + std::string{ detail.data() } + ")";
                }
            case Kind::WindowFocusChanged :
            case Kind::WindowTitleChanged :
            case Kind::WindowCreated :
            case Kind::WindowClosed :
                {
                    const auto* change =
                        std::get_if<grab::WindowChange>( &event.payload );
                    if( change == nullptr )
                    {
                        break;
                    }
                    if( event.kind == Kind::WindowFocusChanged )
                    {
                        return "window \"" +
                               change->title +
                               "\" (" +
                               change->app +
                               ") focused";
                    }
                    if( event.kind == Kind::WindowTitleChanged )
                    {
                        return "window title changed \"" +
                               change->prev_title +
                               "\" to \"" +
                               change->title +
                               "\" (" +
                               change->app +
                               ")";
                    }
                    if( event.kind == Kind::WindowCreated )
                    {
                        return "application \"" +
                               change->app +
                               "\" opened window \"" +
                               change->title +
                               "\"";
                    }
                    std::string closed = "application \"" +
                                         change->app +
                                         "\" closed window \"" +
                                         change->title +
                                         "\"";
                    if( change->duration_s > 0.0 )
                    {
                        std::array<char, detailBufferSize> detail{};
                        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
                        static_cast<void>( std::snprintf( detail.data(),
                                                          detail.size(),
                                                          " (%.1fs)",
                                                          change->duration_s ) );
                        closed += detail.data();
                    }
                    return closed;
                }
            case Kind::A11yButtonClicked :
            case Kind::A11yMenuOpened :
            case Kind::A11yMenuClosed :
            case Kind::A11yFocusChanged :
            case Kind::A11yTextChanged :
            case Kind::A11yStateChanged :
                {
                    const auto* a11y = std::get_if<grab::A11yEvent>( &event.payload );
                    if( a11y == nullptr )
                    {
                        break;
                    }
                    switch( event.kind )
                    {
                        case Kind::A11yButtonClicked :
                            return "button \"" +
                                   a11y->name +
                                   "\" clicked in \"" +
                                   a11y->app +
                                   "\"";
                        case Kind::A11yMenuOpened :
                            return "menu \"" +
                                   a11y->name +
                                   "\" opened in \"" +
                                   a11y->app +
                                   "\"";
                        case Kind::A11yMenuClosed :
                            return "menu \"" +
                                   a11y->name +
                                   "\" closed in \"" +
                                   a11y->app +
                                   "\"";
                        case Kind::A11yFocusChanged :
                            return a11y->role +
                                   " \"" +
                                   a11y->name +
                                   "\" focused in \"" +
                                   a11y->app +
                                   "\"";
                        case Kind::A11yTextChanged :
                            return "text changed in " +
                                   a11y->role +
                                   " \"" +
                                   a11y->name +
                                   "\" (" +
                                   a11y->app +
                                   ")";
                        default :
                            return a11y->role +
                                   " \"" +
                                   a11y->name +
                                   "\" state: " +
                                   a11y->detail +
                                   " (" +
                                   a11y->app +
                                   ")";
                    }
                }
            case Kind::AppTabChanged :
            case Kind::AppContextUpdate :
                {
                    const auto* app =
                        std::get_if<grab::IntegrationEvent>( &event.payload );
                    if( app == nullptr )
                    {
                        break;
                    }
                    if( event.kind == Kind::AppTabChanged )
                    {
                        return "tab \"" + app->title + "\" focused";
                    }
                    return "context updated: \"" + app->detail + "\"";
                }
            case Kind::StateSnapshot :
                {
                    const auto* snapshot =
                        std::get_if<grab::StateSnapshot>( &event.payload );
                    if( snapshot == nullptr )
                    {
                        break;
                    }
                    std::array<char, detailBufferSize> detail{};
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
                    static_cast<void>( std::snprintf(
                        detail.data(),
                        detail.size(),
                        "%.1f KB",
                        static_cast<double>( snapshot->json.size() ) / bytesPerKilobyte
                    ) );
                    return "snapshot (" + std::string{ detail.data() } + ")";
                }
            case Kind::NodeAdded :
            case Kind::NodeRemoved :
            case Kind::NodeChanged :
            case Kind::RelationAdded :
            case Kind::RelationRemoved :
            case Kind::ActiveChildChanged :
                {
                    const auto* graph = std::get_if<grab::GraphChange>( &event.payload );
                    if( graph == nullptr )
                    {
                        break;
                    }
                    switch( event.kind )
                    {
                        case Kind::NodeAdded :
                            return "node added #" + std::to_string( graph->node );
                        case Kind::NodeRemoved :
                            return "node removed #" + std::to_string( graph->node );
                        case Kind::NodeChanged :
                            return "node changed #" + std::to_string( graph->node );
                        case Kind::RelationAdded :
                            return "relation added #" +
                                   std::to_string( graph->node ) +
                                   " -> #" +
                                   std::to_string( graph->related );
                        case Kind::RelationRemoved :
                            return "relation removed #" +
                                   std::to_string( graph->node ) +
                                   " -> #" +
                                   std::to_string( graph->related );
                        default :
                            return "active child changed #" +
                                   std::to_string( graph->previous_active ) +
                                   " -> #" +
                                   std::to_string( graph->node );
                    }
                }
            default :
                break;
        }
        // Unknown kind or payload mismatch: fall back to the single-source
        // wire name so nothing is silently dropped.
        return std::string{ grab::wire_name( event.kind ) };
    }

    // Collapses continuous MouseMove streams into one summary line per
    // window. Discrete kinds never pass through here.
    class MoveCoalescer
    {
        public:

            // Returns the finished summary line when the pending window
            // closes; the new sample opens the next window.
            [[nodiscard]]
            std::optional<std::string>
            feed( const grab::Event& event )
            {
                const auto* move = std::get_if<grab::MouseMove>( &event.payload );
                if( move == nullptr )
                {
                    return std::nullopt;
                }
                std::optional<std::string> line;
                if( pending_.has_value() &&
                    event.timestamp -
                    pending_->first_ts >= coalesceWindowSeconds )
                {
                    line = render( *pending_ );
                    pending_.reset();
                }
                if( !pending_.has_value() )
                {
                    pending_ = Pending{
                        .first_ts      = event.timestamp,
                        .samples       = 0U,
                        .last_position = std::nullopt,
                    };
                }
                ++pending_->samples;
                if( move->position.has_value() )
                {
                    pending_->last_position = *move->position;
                }
                return line;
            }

            [[nodiscard]]
            std::optional<std::string>
            flush()
            {
                if( !pending_.has_value() )
                {
                    return std::nullopt;
                }
                auto line = render( *pending_ );
                pending_.reset();
                return line;
            }

        private:

            struct Pending
            {
                    double                          first_ts = 0.0;
                    std::size_t                     samples  = 0U;
                    std::optional<grab::SpacePoint> last_position;
            };

            [[nodiscard]]
            static std::string
            render( const Pending& pending )
            {
                std::string description;
                if( pending.last_position.has_value() )
                {
                    description =
                        "pointer moved to (" +
                        std::to_string(
                            static_cast<std::int64_t>( pending.last_position->x )
                        ) +
                        ", " +
                        std::to_string(
                            static_cast<std::int64_t>( pending.last_position->y )
                        ) +
                        ")";
                }
                else
                {
                    description = "pointer moved";
                }
                description += " [" + std::to_string( pending.samples ) + " samples]";
                return feed_line( pending.first_ts,
                                  grab::EventKind::MouseMove,
                                  description );
            }

            std::optional<Pending> pending_;
    };

    // ---- consumer -------------------------------------------------------

    class EventLogger
    {
        public:

            // Runs on the session reactor thread only.
            void
            consume( const grab::SubscriptionEvent& item )
            {
                if( const auto* gap = std::get_if<grab::QueueGapMarker>( &item ) )
                {
                    ++gaps_;
                    flush_pending();
                    print( "!! gap: events dropped after seq " +
                           std::to_string( gap->last_delivered_sequence ) );
                    return;
                }
                const auto& event = std::get<grab::Event>( item );
                ++observed_;
                if( event.kind == grab::EventKind::MouseMove )
                {
                    if( auto line = coalescer_.feed( event ); line.has_value() )
                    {
                        print( *line );
                    }
                    return;
                }
                // A discrete event flushes the pending pointer summary first
                // so the feed stays chronological.
                flush_pending();
                print( feed_line( event.timestamp, event.kind, describe( event ) ) );
            }

            void
            flush_pending()
            {
                if( auto line = coalescer_.flush(); line.has_value() )
                {
                    print( *line );
                }
            }

            void
            print( const std::string& line )
            {
                ++printed_;
                std::cout << line << '\n';
                std::cout.flush();
            }

            [[nodiscard]]
            std::size_t
            observed() const noexcept
            {
                return observed_;
            }

            [[nodiscard]]
            std::size_t
            printed() const noexcept
            {
                return printed_;
            }

            [[nodiscard]]
            std::size_t
            gaps() const noexcept
            {
                return gaps_;
            }

            void
            remember_error( grab::Error error )
            {
                const std::scoped_lock lock{ error_mutex_ };
                if( !error_.has_value() )
                {
                    error_ = std::move( error );
                }
            }

            [[nodiscard]]
            std::optional<grab::Error>
            error() const
            {
                const std::scoped_lock lock{ error_mutex_ };
                return error_;
            }

        private:

            MoveCoalescer              coalescer_;
            std::size_t                observed_ = 0U;
            std::size_t                printed_  = 0U;
            std::size_t                gaps_     = 0U;
            mutable std::mutex         error_mutex_;
            std::optional<grab::Error> error_;
    };

    // ---- pump -----------------------------------------------------------

    // Notify -> post -> drain on the session reactor, with a tail-draining
    // stop: the final posted job drains the queue to exhaustion, flushes the
    // coalescer, and fulfils the fence promise, so the last events before
    // Ctrl+C are printed (the fence alone would not drain them).
    class LogPump
    {
        public:

            LogPump( grab::Session&     session,
                     grab::Subscription subscription,
                     EventLogger&       logger ) :
                session_{ &session },
                subscription_{ std::move( subscription ) },
                logger_{ &logger }
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

            // Same accepted narrow race as mouse_snake_trail's TrailPump:
            // set_notify({}) does not join an in-flight notify, so one extra
            // drain can land after the fence; it finds an empty queue and
            // runs while this object is still alive (run() destroys the pump
            // only after Session::close() joins the reactor).
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
                        logger_->flush_pending();
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
                    logger_->remember_error( std::move( posted.error() ) );
                }
            }

            void
            drain()
            {
                scheduled_.store( false );
                while( auto item = subscription_.try_pop_item() )
                {
                    logger_->consume( *item );
                }
            }

            grab::Session*     session_;
            grab::Subscription subscription_;
            EventLogger*       logger_;
            std::atomic_bool   scheduled_{ false };
    };

    // ---- signals --------------------------------------------------------

    [[nodiscard]]
    bool
    block_shutdown_signals(
        // NOLINTNEXTLINE(misc-include-cleaner)
        sigset_t& signals
    ) noexcept
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

    // One 300 ms poll tick; returns true when SIGINT/SIGTERM arrived.
    [[nodiscard]]
    bool
    shutdown_requested(
        // NOLINTNEXTLINE(misc-include-cleaner)
        const sigset_t& signals
    ) noexcept
    {
        const timespec timeout{ .tv_sec = 0, .tv_nsec = pollTickNanos };
        // NOLINTNEXTLINE(misc-include-cleaner): POSIX <signal.h>.
        const int      received = ::sigtimedwait( &signals, nullptr, &timeout );
        return received == SIGINT || received == SIGTERM;
    }

    // ---- main flow ------------------------------------------------------

    [[nodiscard]]
    grab::Result<void>
    run()
    {
        sigset_t signals{};
        // Block BEFORE Session::open(): the session's reactor thread (and
        // every other later-spawned thread) must inherit the mask, or a
        // process-directed SIGINT could be delivered to an unblocked worker.
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

        EventLogger             logger;

        grab::SubscriptionScope scope;    // empty scope + wildcard filter = all kinds
        auto                    subscription =
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

        LogPump pump{ **session, std::move( *subscription ), logger };
        pump.install();

        auto observing = ( *session )->start_observation();
        if( !observing.has_value() )
        {
            return std::unexpected( std::move( observing.error() ) );
        }

        std::cout << "event_logger: observing (Ctrl+C to stop)\n";
        std::cout.flush();

        while( !shutdown_requested( signals ) )
        {
            // Bounded staleness: a pointer that stops moving still gets its
            // final summary within one poll tick.
            auto ticked = ( *session )
                              ->post(
                                  [&logger]
                                  {
                                      logger.flush_pending();
                                  }
                              );
            if( !ticked.has_value() )
            {
                logger.remember_error( std::move( ticked.error() ) );
                break;
            }
        }

        auto stopped = pump.stop();
        ( *session )->close();

        std::cout << "event_logger: " << logger.observed() << " events observed, "
                  << logger.printed() << " lines printed, " << logger.gaps()
                  << " gaps\n";
        std::cout.flush();

        if( !stopped.has_value() )
        {
            return stopped;
        }
        if( auto error = logger.error(); error.has_value() )
        {
            return std::unexpected( std::move( *error ) );
        }
        return {};
    }

}    // namespace

int
main()
{
    auto result = run();
    if( !result.has_value() )
    {
        std::cerr << "event_logger: " << result.error().message << '\n';
        return 1;
    }
    return 0;
}
