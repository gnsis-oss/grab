#include "grab/event.hpp"
#include "grab/origin.hpp"
#include "grab/overlay.hpp"
#include "grab/space.hpp"
#include "grab/watch.hpp"
#include "kernel/presentation/overlay_scene.hpp"
#include "kernel/presentation/trail_animator.hpp"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace grab::kernel::presentation
{
    namespace
    {

        constexpr std::uint64_t failureIncrement = 1U;
        constexpr std::size_t   pathCommandCount = 2U;

        [[nodiscard]]
        constexpr bool
        is_injected_origin( EventOrigin origin ) noexcept
        {
            return origin ==
                   EventOrigin::InjectedSelf ||
                   origin == EventOrigin::InjectedOther;
        }

        [[nodiscard]]
        constexpr bool
        is_trail_origin( EventOrigin origin ) noexcept
        {
            return origin == EventOrigin::Physical || is_injected_origin( origin );
        }

        [[nodiscard]]
        constexpr bool
        is_same_origin_class( EventOrigin left,
                              EventOrigin right ) noexcept
        {
            return ( left == EventOrigin::Physical && right == EventOrigin::Physical ) ||
                   ( is_injected_origin( left ) && is_injected_origin( right ) );
        }

        [[nodiscard]]
        overlay::Shape
        trail_segment( const SpacePoint&     from,
                       const SpacePoint&     to,
                       const overlay::Color& color,
                       const TrailStyle&     style )
        {
            std::vector<overlay::PathCommand> commands;
            commands.reserve( pathCommandCount );
            commands.emplace_back( overlay::MoveTo{ .point = from } );
            commands.emplace_back( overlay::LineTo{ .point = to } );
            return overlay::Shape{
                .geometry =
                    overlay::Path{
                                  .commands = std::move( commands ),
                                  .closed   = false,
                                  },
                .stroke =
                    overlay::StrokeStyle{
                                  .color    = color,
                                  .width_px = style.width_px,
                                  },
                .fill     = std::nullopt,
                .lifetime = overlay::Fade{          .duration = style.fade },
                .band     = overlay::Band::Trail,
            };
        }

    }    // namespace

    TrailAnimator::TrailAnimator( OverlayScene& scene,
                                  TrailStyle    style ) :
        scene_{ scene },
        style_{ style }
    {
    }

    void
    TrailAnimator::consume( const SubscriptionEvent& item )
    {
        if( std::holds_alternative<QueueGapMarker>( item ) )
        {
            break_path();
            return;
        }

        const auto& event = std::get<Event>( item );
        if( event.kind != EventKind::MouseMove )
        {
            return;
        }

        const auto* const motion = std::get_if<MouseMove>( &event.payload );
        if( motion == nullptr || !motion->position.has_value() )
        {
            break_path();
            return;
        }

        if( !is_trail_origin( event.origin ) )
        {
            break_path();
            return;
        }

        const Sample current{
            .position  = *motion->position,
            .origin    = event.origin,
            .timestamp = event.timestamp,
        };
        const auto previous = previous_.value_or( current );
        if( !previous_.has_value() ||
            !is_same_origin_class( previous.origin, current.origin ) ||
            previous.position.space != current.position.space )
        {
            previous_ = current;
            return;
        }

        const auto timestamp_gap = std::chrono::duration<double>{
            current.timestamp - previous.timestamp,
        };
        const auto distance = std::hypot( current.position.x - previous.position.x,
                                          current.position.y - previous.position.y );
        if( timestamp_gap > trailBreakInterval || distance > trailBreakDistancePx )
        {
            previous_ = current;
            return;
        }

        if( distance == 0.0 )
        {
            previous_ = current;
            return;
        }

        const auto& color =
            event.origin == EventOrigin::Physical ? style_.physical : style_.injected;
        const auto added = scene_.add(
            trail_segment( previous.position, current.position, color, style_ )
        );
        if( !added.has_value() )
        {
            scene_add_failures_ += failureIncrement;
        }
        previous_ = current;
    }

    std::uint64_t
    TrailAnimator::scene_add_failure_count() const noexcept
    {
        return scene_add_failures_;
    }

    void
    TrailAnimator::break_path() noexcept
    {
        previous_.reset();
    }

}    // namespace grab::kernel::presentation
