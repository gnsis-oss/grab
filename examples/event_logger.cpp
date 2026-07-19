// event_logger — live terminal feed of everything grab observes.
//
//   ./event_logger [recording-dir] [--socket <path>] [--mouse]
//
// One line per event: "HH:MM:SS.mmm -> <category> event -> <description>".
// Runs until Ctrl+C. Mouse traffic (moves, scrolls, clicks) is hidden from
// the feed unless --mouse is passed; the JSONL recording always keeps it.
//
// Browser wiring: register a native-messaging host in your browser whose
// executable forwards stdio to this socket, e.g.
//   #!/bin/sh
//   exec socat STDIO UNIX-CONNECT:"$XDG_RUNTIME_DIR/grab-event-logger.sock"
// Manifest (Chrome: ~/.config/google-chrome/NativeMessagingHosts/<name>.json,
// Firefox: ~/.mozilla/native-messaging-hosts/<name>.json) points "path" at
// that script; the grab webextension then streams tab events here.

#include "drivers/desktop/x11/window_tracker.hpp"
#include "drivers/semantic/webextension/browser_bridge.hpp"
#include "grab/event.hpp"
#include "grab/event_descriptor.hpp"
#include "grab/result.hpp"
#include "grab/session.hpp"
#include "grab/watch.hpp"
#include "kernel/scheduling/reactor.hpp"
#include "storage/jsonl_sink.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <expected>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/epoll.h>    // NOLINT(misc-include-cleaner): provides EPOLLIN.
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace
{

    constexpr std::size_t   queueCapacity = 8'192U;
    constexpr int           signalSuccess = 0;
    // POSIX timespec::tv_nsec is specified as long.
    // NOLINTNEXTLINE(google-runtime-int)
    constexpr long          pollTickNanos         = 300'000'000L;    // 300 ms
    constexpr int           labelWidth            = 7;               // "browser"
    constexpr double        millisPerSecond       = 1'000.0;
    constexpr std::size_t   timestampBufferSize   = 16U;
    constexpr std::size_t   detailBufferSize      = 32U;
    constexpr double        bytesPerKilobyte      = 1'024.0;
    constexpr double        coalesceWindowSeconds = 0.3;
    constexpr int           listenBacklog         = 4;
    constexpr std::uint32_t epollInEvents         = EPOLLIN;
    constexpr int           invalidFd             = -1;

    [[nodiscard]]
    double
    now_epoch_s()
    {
        const auto duration = std::chrono::system_clock::now().time_since_epoch();
        return std::chrono::duration<double>( duration ).count();
    }

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

    constexpr std::string_view tabTitleKey = "tab_title";

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
                        return "tab \"" + integration_title( *app ) + "\" focused";
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

            // Tick flush: only close windows that have aged out, so a
            // stream that is still accumulating is not chopped mid-window.
            [[nodiscard]]
            std::optional<std::string>
            flush_if_older( double now_s )
            {
                if( !pending_.has_value() ||
                    now_s -
                    pending_->first_ts < coalesceWindowSeconds )
                {
                    return std::nullopt;
                }
                return flush();
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

    // X11 core convention: scroll wheels arrive as button 4 (up), 5 (down),
    // 6 (left), 7 (right) — one press per detent.
    constexpr std::uint32_t scrollButtonFirst = 4U;
    constexpr std::uint32_t scrollButtonLast  = 7U;

    [[nodiscard]]
    constexpr bool
    is_scroll_button( std::uint32_t button )
    {
        return button >= scrollButtonFirst && button <= scrollButtonLast;
    }

    [[nodiscard]]
    constexpr std::string_view
    scroll_direction( std::uint32_t button )
    {
        switch( button )
        {
            case scrollButtonFirst :
                return "up";
            case scrollButtonFirst + 1U :
                return "down";
            case scrollButtonFirst + 2U :
                return "left";
            default :
                return "right";
        }
    }

    // Collapses scroll detents (one button press each) into one summary line
    // per direction per ~300 ms window, like MoveCoalescer does for motion.
    class ScrollCoalescer
    {
        public:

            [[nodiscard]]
            std::optional<std::string>
            feed( const grab::Event& event )
            {
                const auto* click = std::get_if<grab::MouseClick>( &event.payload );
                if( click == nullptr || !is_scroll_button( click->button ) )
                {
                    return std::nullopt;
                }
                std::optional<std::string> line;
                if( pending_.has_value() &&
                    ( pending_->button !=
                      click->button ||
                      event.timestamp -
                      pending_->first_ts >= coalesceWindowSeconds ) )
                {
                    line = render( *pending_ );
                    pending_.reset();
                }
                if( !pending_.has_value() )
                {
                    pending_ = Pending{
                        .first_ts = event.timestamp,
                        .button   = click->button,
                        .detents  = 0U,
                    };
                }
                ++pending_->detents;
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

            // Tick flush: only close windows that have aged out, so a
            // stream that is still accumulating is not chopped mid-window.
            [[nodiscard]]
            std::optional<std::string>
            flush_if_older( double now_s )
            {
                if( !pending_.has_value() ||
                    now_s -
                    pending_->first_ts < coalesceWindowSeconds )
                {
                    return std::nullopt;
                }
                return flush();
            }

        private:

            struct Pending
            {
                    double        first_ts = 0.0;
                    std::uint32_t button   = 0U;
                    std::size_t   detents  = 0U;
            };

            [[nodiscard]]
            static std::string
            render( const Pending& pending )
            {
                const std::string description =
                    "scrolled " +
                    std::string{ scroll_direction( pending.button ) } +
                    " [" +
                    std::to_string( pending.detents ) +
                    " clicks]";
                return feed_line( pending.first_ts,
                                  grab::EventKind::MouseClick,
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
                record( event );
                // Mouse traffic (moves, scrolls, clicks) dominates a live
                // desktop; the feed hides it unless --mouse was passed. The
                // JSONL recording above stays complete either way.
                if( !show_mouse_ && ( event.kind ==
                                      grab::EventKind::MouseMove ||
                                      event.kind == grab::EventKind::MouseClick ) )
                {
                    return;
                }
                if( event.kind == grab::EventKind::MouseMove )
                {
                    // Motion interrupts a pending scroll summary (and vice
                    // versa) so the feed stays chronological.
                    print_if( scroll_coalescer_.flush() );
                    print_if( coalescer_.feed( event ) );
                    return;
                }
                if( const auto* click = std::get_if<grab::MouseClick>( &event.payload );
                    event.kind ==
                    grab::EventKind::MouseClick &&
                    click !=
                    nullptr &&
                    is_scroll_button( click->button ) )
                {
                    print_if( coalescer_.flush() );
                    print_if( scroll_coalescer_.feed( event ) );
                    return;
                }
                // A discrete event flushes the pending summaries first so
                // the feed stays chronological.
                flush_pending();
                print( feed_line( event.timestamp, event.kind, describe( event ) ) );
            }

            void
            flush_pending()
            {
                print_if( coalescer_.flush() );
                print_if( scroll_coalescer_.flush() );
            }

            // The periodic tick only closes aged-out windows; unconditional
            // flushes are reserved for chronology breaks and shutdown.
            void
            tick()
            {
                const double now_s = now_epoch_s();
                print_if( coalescer_.flush_if_older( now_s ) );
                print_if( scroll_coalescer_.flush_if_older( now_s ) );
            }

            void
            print_if( std::optional<std::string> line )
            {
                if( line.has_value() )
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

            void
            attach_sink( grab::storage::JsonlSink* sink ) noexcept
            {
                sink_ = sink;
            }

            void
            set_show_mouse( bool show_mouse ) noexcept
            {
                show_mouse_ = show_mouse;
            }

            // Recording must never kill the live feed: first failure warns
            // once on stderr, stops recording, and is reported at exit.
            void
            record( const grab::Event& event )
            {
                if( sink_ == nullptr || sink_failed_ )
                {
                    return;
                }
                auto written = sink_->write( event );
                if( !written.has_value() )
                {
                    sink_failed_ = true;
                    std::cerr << "event_logger: recording stopped: "
                              << written.error().message << '\n';
                    remember_error( std::move( written.error() ) );
                }
            }

        private:

            MoveCoalescer              coalescer_;
            ScrollCoalescer            scroll_coalescer_;
            std::size_t                observed_    = 0U;
            std::size_t                printed_     = 0U;
            std::size_t                gaps_        = 0U;
            grab::storage::JsonlSink*  sink_        = nullptr;
            bool                       sink_failed_ = false;
            bool                       show_mouse_  = false;
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

    [[nodiscard]]
    std::string
    default_socket_path()
    {
        // NOLINTNEXTLINE(concurrency-mt-unsafe): read once before threads spawn.
        const char*       runtime_dir = std::getenv( "XDG_RUNTIME_DIR" );
        const std::string base =
            runtime_dir != nullptr ? std::string{ runtime_dir } : std::string{ "/tmp" };
        return base + "/grab-event-logger.sock";
    }

    // Accepts native-messaging connections and hands each to a BrowserBridge
    // on the session's reactor/bus. Connection state is touched only on the
    // reactor thread; stop() serializes through a posted fence.
    class BrowserSocket
    {
        public:

            BrowserSocket()                       = default;
            ~BrowserSocket()                      = default;

            BrowserSocket( const BrowserSocket& ) = delete;
            BrowserSocket&
            operator=( const BrowserSocket& ) = delete;

            BrowserSocket( BrowserSocket&& other ) noexcept :
                path_{ std::move( other.path_ ) },
                listen_fd_{
                    std::exchange( other.listen_fd_,
                                   invalidFd ),
                },
                token_{
                    std::exchange( other.token_,
                                   0U ),
                },
                state_{ std::move( other.state_ ) }
            {
            }

            BrowserSocket&
            operator=( BrowserSocket&& ) = delete;

            [[nodiscard]]
            static grab::Result<BrowserSocket>
            open( std::string    path,
                  grab::Session& session )
            {
                sockaddr_un address{};
                if( path.size() >= sizeof( address.sun_path ) )
                {
                    return std::unexpected( grab::Error{
                        .code       = grab::ErrorCode::InvalidArgument,
                        .message    = "socket path too long: " + path,
                        .capability = {},
                        .target     = {},
                        .attempts   = {},
                    } );
                }
                const int fd =
                    ::socket( AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0 );
                if( fd == invalidFd )
                {
                    return std::unexpected( grab::Error{
                        .code = grab::ErrorCode::InternalFault,
                        .message =
                            "socket() failed: " +
                            // NOLINTNEXTLINE(concurrency-mt-unsafe): diagnostic only.
                            std::string{ std::strerror( errno ) },
                        .capability = {},
                        .target     = {},
                        .attempts   = {},
                    } );
                }
                address.sun_family = AF_UNIX;
                // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
                std::strncpy( address.sun_path,
                              path.c_str(),
                              sizeof( address.sun_path ) - 1U );
                ::unlink( path.c_str() );    // stale socket from a prior run
                if( ::bind(
                        fd,
                        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                        reinterpret_cast<const sockaddr*>( &address ),
                        sizeof( address )
                    ) !=
                    0 ||
                    ::listen( fd, listenBacklog ) != 0 )
                {
                    const int saved = errno;
                    ::close( fd );
                    return std::unexpected( grab::Error{
                        .code = grab::ErrorCode::InternalFault,
                        .message =
                            "bind/listen failed on " +
                            path +
                            ": " +
                            // NOLINTNEXTLINE(concurrency-mt-unsafe): diagnostic only.
                            std::string{ std::strerror( saved ) },
                        .capability = {},
                        .target     = {},
                        .attempts   = {},
                    } );
                }

                BrowserSocket socket;
                socket.path_        = std::move( path );
                socket.listen_fd_   = fd;
                socket.state_       = std::make_shared<State>();
                auto* const state   = socket.state_.get();
                auto&       reactor = session.reactor();
                auto&       bus     = session.bus();
                socket.token_ =
                    reactor.add_fd( fd,
                                    epollInEvents,
                                    [fd, state, &reactor, &bus]( std::uint32_t )
                                    {
                                        accept_pending( fd, *state, reactor, bus );
                                    } );
                return socket;
            }

            void
            stop( grab::Session& session )
            {
                if( listen_fd_ == invalidFd )
                {
                    return;
                }
                session.reactor().remove_fd( token_ );
                // Serialize with any in-flight accept callback.
                std::promise<void> fence;
                auto               reached = fence.get_future();
                auto               posted  = session.post(
                    [this, &fence]
                    {
                        for( auto& connection : state_->connections )
                        {
                            connection.bridge.stop();
                            ::close( connection.fd );
                        }
                        state_->connections.clear();
                        fence.set_value();
                    }
                );
                if( posted.has_value() )
                {
                    reached.get();
                }
                ::close( listen_fd_ );
                ::unlink( path_.c_str() );
                listen_fd_ = invalidFd;
            }

        private:

            struct Connection
            {
                    int                                                  fd;
                    grab::drivers::semantic::webextension::BrowserBridge bridge;
            };

            struct State
            {
                    std::vector<Connection> connections;    // reactor thread only
            };

            static void
            accept_pending( int                  listen_fd,
                            State&               state,
                            grab::core::Reactor& reactor,
                            grab::EventBus&      bus )
            {
                for( ;; )
                {
                    const int fd = ::accept4( listen_fd,
                                              nullptr,
                                              nullptr,
                                              SOCK_NONBLOCK | SOCK_CLOEXEC );
                    if( fd == invalidFd )
                    {
                        return;    // EAGAIN or transient error: wait for next EPOLLIN
                    }
                    auto bridge =
                        grab::drivers::semantic::webextension::BrowserBridge::start(
                            fd,
                            reactor,
                            bus
                        );
                    if( !bridge.has_value() )
                    {
                        ::close( fd );
                        continue;
                    }
                    state.connections.push_back(
                        Connection{ .fd = fd, .bridge = std::move( *bridge ) }
                    );
                }
            }

            std::string   path_{};    // NOLINT(readability-redundant-member-init)
            int           listen_fd_ = invalidFd;
            std::uint64_t token_     = 0U;
            std::shared_ptr<State>
                state_{};    // NOLINT(readability-redundant-member-init)
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

    // The live phase: everything that runs while the pump is installed and
    // observation is on. Producers started here are stopped here on every
    // path; failures return WITHOUT touching pump/logger teardown, so run()
    // can always fence the pump before destroying it (the same invariant
    // mouse_snake_trail routes through sweep_and_hold()).
    [[nodiscard]]
    grab::Result<void>
    observe_and_log( grab::Session&     session,
                     EventLogger&       logger,
                     const std::string& socket_path,
                     const sigset_t&    signals )    // NOLINT(misc-include-cleaner)
    {
        // The owning Session composes input + AT-SPI + tree sources but no
        // WindowTracker; start one on the session's own reactor and bus
        // (the public composition seam) so os-category events join the feed.
        auto tracker =
            grab::drivers::desktop::x11::WindowTracker::start( nullptr,
                                                               session.reactor(),
                                                               session.bus() );
        if( !tracker.has_value() )
        {
            return std::unexpected( std::move( tracker.error() ) );
        }

        auto browser_socket = BrowserSocket::open( socket_path, session );
        if( !browser_socket.has_value() )
        {
            tracker->stop();
            return std::unexpected( std::move( browser_socket.error() ) );
        }
        std::cout << "event_logger: browser socket at " << socket_path << '\n';
        std::cout << "event_logger: observing (Ctrl+C to stop)\n";
        std::cout.flush();

        while( !shutdown_requested( signals ) )
        {
            // Bounded staleness: a pointer that stops moving still gets its
            // final summary within one poll tick.
            auto ticked = session.post(
                [&logger]
                {
                    logger.tick();
                }
            );
            if( !ticked.has_value() )
            {
                logger.remember_error( std::move( ticked.error() ) );
                break;
            }
        }

        browser_socket->stop( session );
        tracker->stop();
        return {};
    }

    [[nodiscard]]
    grab::Result<void>
    // NOLINTNEXTLINE(readability-function-size): lifecycle is intentionally linear.
    run( std::span<char*> args )
    {
        std::filesystem::path recording_dir{ "event-log" };
        std::string           socket_path = default_socket_path();
        bool                  show_mouse  = false;
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
            else if( arg == "--mouse" )
            {
                show_mouse = true;
            }
            else if( index == 0U )
            {
                recording_dir = arg;
            }
        }

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

        EventLogger logger;

        auto        sink = grab::storage::JsonlSink::open(
            grab::storage::JsonlOptions{ .dir = recording_dir }
        );
        if( !sink.has_value() )
        {
            return std::unexpected( std::move( sink.error() ) );
        }
        logger.attach_sink( &*sink );
        logger.set_show_mouse( show_mouse );
        if( !show_mouse )
        {
            std::cout << "event_logger: mouse events hidden"
                         " (pass --mouse to show; still recorded)\n";
            std::cout.flush();
        }

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

        // pump/logger are live (install() ran above): from here on, every
        // exit path must run pump.stop()'s fence before pump/logger are
        // destroyed, or a queued drain() could touch freed memory. Route
        // every early return of the live phase through observe_and_log()
        // instead of returning directly, so that invariant holds
        // unconditionally (same structure as mouse_snake_trail).
        auto observed = observe_and_log( **session, logger, socket_path, signals );
        auto stopped  = pump.stop();
        ( *session )->close();

        auto flushed = sink->flush();
        sink->close();
        if( !flushed.has_value() && !logger.error().has_value() )
        {
            logger.remember_error( std::move( flushed.error() ) );
        }

        std::cout << "event_logger: " << logger.observed() << " events observed, "
                  << logger.printed() << " lines printed, " << logger.gaps()
                  << " gaps, recording: " << recording_dir.string() << '\n';
        std::cout.flush();

        if( !observed.has_value() )
        {
            return observed;
        }
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
main( int   argc,
      char* argv[] )
{
    auto result =
        run( std::span{ argv, static_cast<std::size_t>( argc ) }.subspan( 1 ) );
    if( !result.has_value() )
    {
        std::cerr << "event_logger: " << result.error().message << '\n';
        return 1;
    }
    return 0;
}
