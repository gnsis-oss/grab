#include "grab/overlay.hpp"
#include "grab/space.hpp"
#include "kernel/presentation/overlay_draw.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numeric>
#include <optional>
#include <utility>

namespace grab::overlay
{
    namespace
    {

        struct DrawBounds
        {
                double x{};
                double y{};
                double width{};
                double height{};
        };

        constexpr double ellipseDiameterRatio = 2.0;

        [[nodiscard]]
        DrawBounds
        draw_bounds( SpacePoint first,
                     SpacePoint second ) noexcept
        {
            const auto left   = std::min( first.x, second.x );
            const auto right  = std::max( first.x, second.x );
            const auto top    = std::min( first.y, second.y );
            const auto bottom = std::max( first.y, second.y );
            return DrawBounds{
                .x      = left,
                .y      = top,
                .width  = right - left,
                .height = bottom - top,
            };
        }

        [[nodiscard]]
        bool
        point_is_finite( SpacePoint point ) noexcept
        {
            return std::isfinite( point.x ) && std::isfinite( point.y );
        }

        [[nodiscard]]
        bool
        bounds_are_drawable( const DrawBounds& bounds ) noexcept
        {
            return std::isfinite( bounds.width ) &&
                   std::isfinite( bounds.height ) &&
                   bounds.width >=
                   minDrawExtentPx &&
                   bounds.height >= minDrawExtentPx;
        }

        [[nodiscard]]
        overlay::Shape
        styled_shape( overlay::Geometry geometry,
                      const DrawStyle&  style )
        {
            overlay::Shape shape;
            shape.geometry = std::move( geometry );
            if( style.filled )
            {
                shape.fill = overlay::FillStyle{ .color = style.color };
            }
            else
            {
                shape.stroke = overlay::StrokeStyle{
                    .color    = style.color,
                    .width_px = style.stroke_px,
                };
            }
            return shape;
        }

    }    // namespace

    void
    DrawInteraction::begin( DrawKind         kind,
                            SpacePoint       at,
                            const DrawStyle& style )
    {
        cancel();
        kind_       = kind;
        style_      = style;
        began_at_   = at;
        current_at_ = at;
        if( kind_ == DrawKind::Path )
        {
            path_points_.reserve( maxPathSamples );
            path_points_.push_back( at );
        }
        active_ = true;
    }

    std::optional<overlay::Shape>
    DrawInteraction::update( SpacePoint at )
    {
        if( !can_sample( at ) )
        {
            return std::nullopt;
        }
        sample( at );
        return current_shape();
    }

    std::optional<overlay::Shape>
    DrawInteraction::commit( SpacePoint at )
    {
        if( !active_ )
        {
            return std::nullopt;
        }
        if( can_sample( at ) )
        {
            sample( at );
        }
        auto result = current_shape();
        cancel();
        return result;
    }

    void
    DrawInteraction::cancel() noexcept
    {
        kind_       = DrawKind::Rectangle;
        style_      = DrawStyle{};
        began_at_   = SpacePoint{};
        current_at_ = SpacePoint{};
        path_points_.clear();
        active_ = false;
    }

    bool
    DrawInteraction::active() const noexcept
    {
        return active_;
    }

    DrawKind
    DrawInteraction::kind() const noexcept
    {
        return kind_;
    }

    bool
    DrawInteraction::can_sample( SpacePoint at ) const noexcept
    {
        return active_ && at.space == began_at_.space && point_is_finite( at );
    }

    void
    DrawInteraction::sample( SpacePoint at )
    {
        if( kind_ != DrawKind::Path )
        {
            current_at_ = at;
            return;
        }
        if( path_points_.size() >= maxPathSamples )
        {
            return;
        }
        const auto& previous = path_points_.back();
        if( std::hypot( at.x - previous.x, at.y - previous.y ) < minPathSampleSpacingPx )
        {
            return;
        }
        path_points_.push_back( at );
    }

    std::optional<overlay::Shape>
    DrawInteraction::current_shape() const
    {
        if( !active_ || !point_is_finite( began_at_ ) )
        {
            return std::nullopt;
        }
        if( kind_ == DrawKind::Path )
        {
            if( path_points_.size() < 2U )
            {
                return std::nullopt;
            }
            overlay::Path path;
            path.commands.reserve( path_points_.size() );
            path.commands.emplace_back(
                overlay::MoveTo{ .point = path_points_.front() }
            );
            std::transform( std::next( path_points_.begin() ),
                            path_points_.end(),
                            std::back_inserter( path.commands ),
                            []( SpacePoint point ) -> overlay::PathCommand
                            {
                                return overlay::LineTo{ .point = point };
                            } );
            return styled_shape( std::move( path ), style_ );
        }

        const auto bounds = draw_bounds( began_at_, current_at_ );
        if( !bounds_are_drawable( bounds ) )
        {
            return std::nullopt;
        }
        if( kind_ == DrawKind::Rectangle )
        {
            return styled_shape(
                overlay::Rect{
                    .bounds =
                        SpaceRect{
                                  .x     = bounds.x,
                                  .y     = bounds.y,
                                  .w     = bounds.width,
                                  .h     = bounds.height,
                                  .space = began_at_.space,
                                  },
            },
                style_
            );
        }
        return styled_shape(
            overlay::Ellipse{
                .center =
                    SpacePoint{
                               .x     = std::midpoint( began_at_.x, current_at_.x ),
                               .y     = std::midpoint( began_at_.y, current_at_.y ),
                               .space = began_at_.space,
                               },
                .radius_x = bounds.width / ellipseDiameterRatio,
                .radius_y = bounds.height / ellipseDiameterRatio,
        },
            style_
        );
    }

}    // namespace grab::overlay
