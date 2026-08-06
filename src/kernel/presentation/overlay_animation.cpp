#include "grab/overlay.hpp"
#include "kernel/presentation/overlay_animation.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <variant>

namespace grab::kernel::presentation
{
    namespace
    {

        constexpr double fullyHidden         = 0.0;
        constexpr double fullyVisible        = 1.0;
        constexpr double halfProgress        = 0.5;
        constexpr double quadraticInOutScale = 2.0;
        constexpr double cubicInOutScale     = 4.0;

        [[nodiscard]]
        bool
        valid_easing( overlay::Easing easing ) noexcept
        {
            switch( easing )
            {
                case overlay::Easing::Linear :
                case overlay::Easing::InQuad :
                case overlay::Easing::OutQuad :
                case overlay::Easing::InOutQuad :
                case overlay::Easing::InCubic :
                case overlay::Easing::OutCubic :
                case overlay::Easing::InOutCubic :
                    return true;
            }
            return false;
        }

        [[nodiscard]]
        bool
        valid_axis( overlay::Axis axis ) noexcept
        {
            switch( axis )
            {
                case overlay::Axis::X :
                case overlay::Axis::Y :
                    return true;
            }
            return false;
        }

        [[nodiscard]]
        bool
        valid_edge( overlay::Edge edge ) noexcept
        {
            switch( edge )
            {
                case overlay::Edge::Min :
                case overlay::Edge::Max :
                    return true;
            }
            return false;
        }

        [[nodiscard]]
        bool
        valid_channel( const overlay::Channel& channel ) noexcept
        {
            return valid_easing( channel.easing ) &&
                   channel.duration >= std::chrono::milliseconds::zero();
        }

        [[nodiscard]]
        bool
        unit_interval( double value ) noexcept
        {
            return std::isfinite( value ) &&
                   value >=
                   fullyHidden &&
                   value <= fullyVisible;
        }

        [[nodiscard]]
        double
        easing_value( overlay::Easing easing,
                      double          progress ) noexcept
        {
            const auto inverse = fullyVisible - progress;
            switch( easing )
            {
                case overlay::Easing::Linear :
                    return progress;
                case overlay::Easing::InQuad :
                    return progress * progress;
                case overlay::Easing::OutQuad :
                    return fullyVisible - ( inverse * inverse );
                case overlay::Easing::InOutQuad :
                    if( progress < halfProgress )
                    {
                        return quadraticInOutScale * progress * progress;
                    }
                    return fullyVisible - ( quadraticInOutScale * inverse * inverse );
                case overlay::Easing::InCubic :
                    return progress * progress * progress;
                case overlay::Easing::OutCubic :
                    return fullyVisible - ( inverse * inverse * inverse );
                case overlay::Easing::InOutCubic :
                    if( progress < halfProgress )
                    {
                        return cubicInOutScale * progress * progress * progress;
                    }
                    return fullyVisible -
                           ( cubicInOutScale * inverse * inverse * inverse );
            }
            return progress;
        }

        [[nodiscard]]
        double
        channel_progress( const overlay::Channel&   channel,
                          std::chrono::milliseconds elapsed ) noexcept
        {
            if( elapsed < std::chrono::milliseconds::zero() )
            {
                return fullyHidden;
            }
            if( channel.duration <= std::chrono::milliseconds::zero() )
            {
                return fullyVisible;
            }
            const auto progress = static_cast<double>( elapsed.count() ) /
                                  static_cast<double>( channel.duration.count() );
            return easing_value( channel.easing,
                                 std::clamp( progress, fullyHidden, fullyVisible ) );
        }

        [[nodiscard]]
        double
        interpolate( double from,
                     double to,
                     double progress ) noexcept
        {
            return from + ( ( to - from ) * progress );
        }

        [[nodiscard]]
        double
        lifetime_opacity( const overlay::ShapeRecord& record,
                          std::chrono::milliseconds   now ) noexcept
        {
            if( std::holds_alternative<overlay::Persistent>( record.shape.lifetime ) )
            {
                return fullyVisible;
            }

            const auto elapsed = now - record.started_at;
            if( const auto* ttl = std::get_if<overlay::Ttl>( &record.shape.lifetime ) )
            {
                return elapsed <
                               ttl->duration &&
                               ttl->duration > std::chrono::milliseconds::zero()
                         ? fullyVisible
                         : fullyHidden;
            }

            const auto* fade = std::get_if<overlay::Fade>( &record.shape.lifetime );
            if( fade == nullptr || fade->duration <= std::chrono::milliseconds::zero() )
            {
                return fullyHidden;
            }
            const auto progress = static_cast<double>( elapsed.count() ) /
                                  static_cast<double>( fade->duration.count() );
            return std::clamp( fullyVisible - progress, fullyHidden, fullyVisible );
        }

    }    // namespace

    bool
    valid_animation( const overlay::AnimationSpec& animation ) noexcept
    {
        if( animation.scale.has_value() && ( !valid_channel( *animation.scale ) ||
                                             !std::isfinite( animation.scale->from ) ||
                                             !std::isfinite( animation.scale->to ) ||
                                             animation.scale->from <
                                             fullyHidden ||
                                             animation.scale->to < fullyHidden ) )
        {
            return false;
        }
        if( animation.opacity.has_value() &&
            ( !valid_channel( *animation.opacity ) ||
              !unit_interval( animation.opacity->from ) ||
              !unit_interval( animation.opacity->to ) ) )
        {
            return false;
        }
        if( animation.translate.has_value() &&
            ( !valid_channel( *animation.translate ) ||
              !std::isfinite( animation.translate->dx ) ||
              !std::isfinite( animation.translate->dy ) ) )
        {
            return false;
        }
        if( animation.reveal.has_value() &&
            ( !valid_channel( *animation.reveal ) ||
              !valid_axis( animation.reveal->axis ) ||
              !valid_edge( animation.reveal->from_edge ) ||
              !unit_interval( animation.reveal->from ) ||
              !unit_interval( animation.reveal->to ) ) )
        {
            return false;
        }
        return true;
    }

    std::chrono::milliseconds
    animation_duration( const overlay::AnimationSpec& animation ) noexcept
    {
        auto       result  = std::chrono::milliseconds::zero();
        const auto include = [&result]( const auto& channel )
        {
            if( channel.has_value() )
            {
                result = std::max( result, channel->duration );
            }
        };
        include( animation.scale );
        include( animation.opacity );
        include( animation.translate );
        include( animation.reveal );
        return result;
    }

    EvaluatedAnimation
    evaluate_animation( const overlay::AnimationSpec& animation,
                        std::chrono::milliseconds     elapsed ) noexcept
    {
        EvaluatedAnimation result;
        result.duration = animation_duration( animation );
        result.complete =
            elapsed >= result.duration && elapsed >= std::chrono::milliseconds::zero();

        if( animation.scale.has_value() )
        {
            result.scale = interpolate( animation.scale->from,
                                        animation.scale->to,
                                        channel_progress( *animation.scale, elapsed ) );
        }
        if( animation.opacity.has_value() )
        {
            result.opacity =
                interpolate( animation.opacity->from,
                             animation.opacity->to,
                             channel_progress( *animation.opacity, elapsed ) );
        }
        if( animation.translate.has_value() )
        {
            const auto progress = channel_progress( *animation.translate, elapsed );
            result.translate_x  = animation.translate->dx * progress;
            result.translate_y  = animation.translate->dy * progress;
        }
        if( animation.reveal.has_value() )
        {
            result.reveal = EvaluatedReveal{
                .axis      = animation.reveal->axis,
                .from_edge = animation.reveal->from_edge,
                .fraction =
                    interpolate( animation.reveal->from,
                                 animation.reveal->to,
                                 channel_progress( *animation.reveal, elapsed ) ),
            };
        }
        return result;
    }

    EvaluatedAnimation
    evaluate_animation( const overlay::ShapeRecord& record,
                        std::chrono::milliseconds   now ) noexcept
    {
        if( !record.shape.animation.has_value() )
        {
            return {};
        }
        return evaluate_animation( *record.shape.animation, now - record.started_at );
    }

    double
    evaluate_opacity( const overlay::ShapeRecord& record,
                      std::chrono::milliseconds   now ) noexcept
    {
        return lifetime_opacity( record, now ) *
               evaluate_animation( record, now ).opacity;
    }

    AnimationRect
    reveal_clip( AnimationRect          bounds,
                 const EvaluatedReveal& reveal ) noexcept
    {
        const auto fraction = std::clamp( reveal.fraction, fullyHidden, fullyVisible );
        if( reveal.axis == overlay::Axis::X )
        {
            const auto visible_width = bounds.width * fraction;
            if( reveal.from_edge == overlay::Edge::Max )
            {
                bounds.x += bounds.width - visible_width;
            }
            bounds.width = visible_width;
            return bounds;
        }

        const auto visible_height = bounds.height * fraction;
        if( reveal.from_edge == overlay::Edge::Max )
        {
            bounds.y += bounds.height - visible_height;
        }
        bounds.height = visible_height;
        return bounds;
    }

}    // namespace grab::kernel::presentation
