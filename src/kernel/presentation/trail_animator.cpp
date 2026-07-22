#include "grab/event.hpp"
#include "grab/origin.hpp"
#include "grab/overlay.hpp"
#include "grab/space.hpp"
#include "grab/watch.hpp"
#include "kernel/presentation/overlay_scene.hpp"
#include "kernel/presentation/trail_animator.hpp"

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

        if( event.origin !=
            EventOrigin::Physical &&
            event.origin != EventOrigin::InjectedSelf )
        {
            break_path();
            return;
        }

        const Sample current{
            .position = *motion->position,
            .origin   = event.origin,
        };
        const auto previous = previous_.value_or( current );
        if( !previous_.has_value() ||
            previous.origin !=
            current.origin ||
            previous.position.space != current.position.space )
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
