#include "grab/event.hpp"
#include "grab/image.hpp"
#include "grab/input.hpp"
#include "grab/origin.hpp"
#include "grab/overlay.hpp"
#include "grab/result.hpp"
#include "grab/screen.hpp"
#include "grab/session.hpp"
#include "grab/space.hpp"
#include "grab/watch.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <future>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace
{

    // Sweep geometry. The pass count is odd so the final pass travels
    // left->right and the sweep ends at the inset bottom-right corner.
    constexpr std::int32_t              screenMarginPx = 40;
    constexpr std::size_t               snakePassCount = 9U;
    constexpr std::int32_t              sweepStepPx    = 12;
    constexpr std::chrono::milliseconds sweepStepInterval{ 6 };

    // Trail style — the built-in trail palette: injected blue, physical red.
    constexpr std::uint8_t channelMax = std::numeric_limits<std::uint8_t>::max();
    constexpr grab::overlay::Color
        injectedTrailColor{ .r = 0U, .g = 0U, .b = channelMax, .a = channelMax };
    constexpr grab::overlay::Color
        physicalTrailColor{ .r = channelMax, .g = 0U, .b = 0U, .a = channelMax };
    constexpr std::chrono::milliseconds trailFade{ 1'200 };
    constexpr float                     trailWidthPx        = 3.0F;
    constexpr std::size_t               segmentCommandCount = 2U;
    // Hold after the sweep so the last segments fade out on screen.
    constexpr std::chrono::milliseconds endHold{ 1'500 };

    struct PixelPoint
    {
            std::int32_t x = 0;
            std::int32_t y = 0;

            friend constexpr bool
            operator==( const PixelPoint&,
                        const PixelPoint& ) = default;
    };

    [[nodiscard]]
    constexpr std::array<PixelPoint,
                         snakePassCount * 2U>
    snake_waypoints( std::int32_t width,
                     std::int32_t height )
    {
        std::array<PixelPoint, snakePassCount * 2U> points{};
        const std::int32_t                          left   = screenMarginPx;
        const std::int32_t                          right  = width - screenMarginPx;
        const std::int32_t                          top    = screenMarginPx;
        const std::int32_t                          bottom = height - screenMarginPx;
        const std::int32_t                          spanY  = bottom - top;
        const auto lastPass = static_cast<std::int32_t>( snakePassCount ) - 1;
        for( std::size_t pass = 0U; pass < snakePassCount; ++pass )
        {
            const std::int32_t row =
                top + ( ( static_cast<std::int32_t>( pass ) * spanY ) / lastPass );
            const bool rightward = pass % 2U == 0U;
            points.at( pass * 2U ) =
                PixelPoint{ .x = rightward ? left : right, .y = row };
            points.at( ( pass * 2U ) + 1U ) =
                PixelPoint{ .x = rightward ? right : left, .y = row };
        }
        return points;
    }

    // Compile-time contract on a reference display: the sweep starts at the
    // inset top-left corner, ends at the inset bottom-right corner, and
    // alternates direction (pass 0 rightward, pass 1 leftward).
    constexpr std::int32_t referenceWidth  = 1'920;
    constexpr std::int32_t referenceHeight = 1'080;
    constexpr auto         referenceWaypoints =
        snake_waypoints( referenceWidth, referenceHeight );
    static_assert( snakePassCount % 2U == 1U,
                   "odd pass count so the final pass ends bottom-right" );
    static_assert( referenceWaypoints.front() == PixelPoint{
                                                     .x = screenMarginPx,
                                                     .y = screenMarginPx,
                                                 } );
    static_assert( referenceWaypoints.back() ==
                   PixelPoint{
                       .x = referenceWidth - screenMarginPx,
                       .y = referenceHeight - screenMarginPx,
                   } );
    static_assert( referenceWaypoints.at( 1U ).x == referenceWidth - screenMarginPx );
    static_assert( referenceWaypoints.at( 2U ).x == referenceWidth - screenMarginPx );
    static_assert( referenceWaypoints.at( 3U ).x == screenMarginPx );

    [[nodiscard]]
    grab::Result<void>
    move_line( grab::Input&      input,
               const PixelPoint& from,
               const PixelPoint& to )
    {
        const std::int32_t deltaX   = to.x - from.x;
        const std::int32_t deltaY   = to.y - from.y;
        const std::int32_t distance = std::max( std::abs( deltaX ), std::abs( deltaY ) );
        const std::int32_t steps    = std::max( distance / sweepStepPx, 1 );
        for( std::int32_t step = 1; step <= steps; ++step )
        {
            const std::int32_t x     = from.x + ( ( deltaX * step ) / steps );
            const std::int32_t y     = from.y + ( ( deltaY * step ) / steps );
            auto               moved = input.move( static_cast<std::int16_t>( x ),
                                                   static_cast<std::int16_t>( y ) );
            if( !moved.has_value() )
            {
                return moved;
            }
            std::this_thread::sleep_for( sweepStepInterval );
        }
        return {};
    }

    // Deliberate divergence from the kernel TrailAnimator: it draws ALL
    // positioned moves regardless of origin (our own injections arrive as
    // InjectedOther because the example's Input connection is not the
    // session seat — the kernel animator would skip them and no snake would
    // appear). It breaks the path on origin or coordinate-space changes
    // instead of mixing them into one segment.
    class TrailRenderer
    {
        public:

            explicit TrailRenderer( grab::Overlay& overlay ) :
                overlay_{ &overlay }
            {
            }

            // Runs on the session reactor thread only.
            void
            consume( const grab::SubscriptionEvent& item )
            {
                if( std::holds_alternative<grab::QueueGapMarker>( item ) )
                {
                    previous_.reset();
                    return;
                }
                const auto& event = std::get<grab::Event>( item );
                if( event.kind != grab::EventKind::MouseMove )
                {
                    return;
                }
                const auto* const motion =
                    std::get_if<grab::MouseMove>( &event.payload );
                if( motion == nullptr || !motion->position.has_value() )
                {
                    previous_.reset();
                    return;
                }
                const grab::SpacePoint current = *motion->position;
                if( !previous_.has_value() ||
                    previous_->position.space !=
                    current.space ||
                    previous_->origin != event.origin )
                {
                    previous_ = Sample{ .position = current, .origin = event.origin };
                    return;
                }
                auto added = overlay_->add(
                    trail_segment( previous_->position, current, event.origin )
                );
                if( !added.has_value() )
                {
                    remember_error( std::move( added.error() ) );
                }
                previous_ = Sample{ .position = current, .origin = event.origin };
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

            struct Sample
            {
                    grab::SpacePoint  position{};
                    grab::EventOrigin origin{ grab::EventOrigin::Unknown };
            };

            [[nodiscard]]
            static grab::overlay::Shape
            trail_segment( const grab::SpacePoint& from,
                           const grab::SpacePoint& to,
                           grab::EventOrigin       origin )
            {
                std::vector<grab::overlay::PathCommand> commands;
                commands.reserve( segmentCommandCount );
                commands.emplace_back( grab::overlay::MoveTo{ .point = from } );
                commands.emplace_back( grab::overlay::LineTo{ .point = to } );
                const grab::overlay::Color color = origin == grab::EventOrigin::Physical
                                                     ? physicalTrailColor
                                                     : injectedTrailColor;
                return grab::overlay::Shape{
                    .geometry =
                        grab::overlay::Path{
                                            .commands = std::move( commands ),
                                            .closed   = false,
                                            },
                    .stroke =
                        grab::overlay::StrokeStyle{
                                            .color    = color,
                                            .width_px = trailWidthPx,
                                            },
                    .fill     = std::nullopt,
                    .lifetime = grab::overlay::Fade{           .duration = trailFade },
                    .band     = grab::overlay::Band::Trail,
                };
            }

            grab::Overlay*             overlay_;
            std::optional<Sample>      previous_;
            mutable std::mutex         error_mutex_;
            std::optional<grab::Error> error_;
    };

    // Notify -> post -> drain, with a shutdown fence.
    class TrailPump
    {
        public:

            TrailPump( grab::Session&     session,
                       grab::Subscription subscription,
                       TrailRenderer&     renderer ) :
                session_{ &session },
                subscription_{ std::move( subscription ) },
                renderer_{ &renderer }
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

            // Stops event flow and waits until every already-queued drain has
            // executed, so no reactor job touches this object afterwards.
            // Accepted narrow race: set_notify({}) does not join an
            // in-flight notify callback, so one extra drain can still get
            // posted after the fence. Harmless here: the reactor runs its
            // remaining jobs before honoring stop, Session::close() joins
            // the reactor thread, and run() destroys the pump only after
            // close() returns — so the straggler (which finds an empty
            // queue) always runs while this object is alive.
            [[nodiscard]]
            grab::Result<void>
            stop()
            {
                subscription_.set_notify( {} );
                session_->stop_observation();
                return reactor_fence();
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
                    renderer_->remember_error( std::move( posted.error() ) );
                }
            }

            void
            drain()
            {
                // Re-arm before draining: a notify that lands mid-drain
                // schedules a follow-up drain instead of being lost; the worst
                // case is one extra drain that finds an empty queue.
                scheduled_.store( false );
                while( auto item = subscription_.try_pop_item() )
                {
                    renderer_->consume( *item );
                }
            }

            [[nodiscard]]
            grab::Result<void>
            reactor_fence()
            {
                std::promise<void> fence;
                auto               reached = fence.get_future();
                auto               posted  = session_->post(
                    [&fence]
                    {
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

            grab::Session*     session_;
            grab::Subscription subscription_;
            TrailRenderer*     renderer_;
            std::atomic_bool   scheduled_{ false };
    };

    // Everything that happens once observation is live: open Input, sweep the
    // snake, flush, and hold for the fade. Deliberately touches nothing
    // pump/renderer-related, so every early return here is safe to take
    // without a completed TrailPump::stop() fence first -- run() always calls
    // pump.stop() right after this returns, success or failure.
    [[nodiscard]]
    grab::Result<void>
    sweep_and_hold( grab::Overlay& overlay,
                    std::int32_t   width,
                    std::int32_t   height )
    {
        auto input = grab::Input::open();
        if( !input.has_value() )
        {
            return std::unexpected( std::move( input.error() ) );
        }

        const auto waypoints = snake_waypoints( width, height );
        auto placed = input->move( static_cast<std::int16_t>( waypoints.front().x ),
                                   static_cast<std::int16_t>( waypoints.front().y ) );
        if( !placed.has_value() )
        {
            return placed;
        }
        for( std::size_t index = 1U; index < waypoints.size(); ++index )
        {
            auto swept =
                move_line( *input, waypoints.at( index - 1U ), waypoints.at( index ) );
            if( !swept.has_value() )
            {
                return swept;
            }
        }

        // Fence the last segments onto the screen, then hold while they fade.
        auto flushed = overlay.flush();
        if( !flushed.has_value() )
        {
            return flushed;
        }
        std::this_thread::sleep_for( endHold );
        return {};
    }

    [[nodiscard]]
    grab::Result<void>
    run()
    {
        auto session = grab::Session::open();
        if( !session.has_value() )
        {
            return std::unexpected( std::move( session.error() ) );
        }

        auto overlay = ( *session )->overlay();
        if( !overlay.has_value() )
        {
            return std::unexpected( std::move( overlay.error() ) );
        }
        // Phase 1 renders in the display-global space; observed positions
        // arrive in the same space, so segments need no transform. space() is
        // queried to fail fast if that invariant ever breaks.
        auto space = ( *overlay )->space();
        if( !space.has_value() )
        {
            return std::unexpected( std::move( space.error() ) );
        }

        auto screen = grab::Screen::open();
        if( !screen.has_value() )
        {
            return std::unexpected( std::move( screen.error() ) );
        }
        auto frame = screen->display();
        if( !frame.has_value() )
        {
            return std::unexpected( std::move( frame.error() ) );
        }
        const auto              width  = static_cast<std::int32_t>( frame->width );
        const auto              height = static_cast<std::int32_t>( frame->height );

        TrailRenderer           renderer{ **overlay };

        grab::SubscriptionScope scope;
        scope.kinds       = { grab::EventKind::MouseMove };
        auto subscription = ( *session )->watch( std::move( scope ) );
        if( !subscription.has_value() )
        {
            return std::unexpected( std::move( subscription.error() ) );
        }

        TrailPump pump{ **session, std::move( *subscription ), renderer };
        pump.install();

        auto observing = ( *session )->start_observation();
        if( !observing.has_value() )
        {
            return std::unexpected( std::move( observing.error() ) );
        }

        // pump/renderer are live (install() ran above): from here on, every
        // exit path must run pump.stop()'s reactor fence before pump/renderer
        // are destroyed, or a queued drain() could touch freed memory. Route
        // every early return in the sweep through sweep_and_hold() instead of
        // returning directly, so that invariant holds unconditionally.
        auto swept   = sweep_and_hold( **overlay, width, height );
        auto stopped = pump.stop();
        ( *session )->close();

        if( !swept.has_value() )
        {
            return swept;
        }
        if( !stopped.has_value() )
        {
            return stopped;
        }
        if( auto error = renderer.error(); error.has_value() )
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
        std::cerr << "mouse_snake_trail: " << result.error().message << '\n';
        return 1;
    }
    return 0;
}
