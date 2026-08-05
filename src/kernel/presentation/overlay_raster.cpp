#include "grab/geometry/curve.hpp"
#include "grab/geometry/point.hpp"
#include "grab/geometry/rectangle.hpp"
#include "grab/geometry/size.hpp"
#include "grab/image.hpp"
#include "grab/overlay.hpp"
#include "grab/result.hpp"
#include "grab/space.hpp"
#include "kernel/presentation/overlay_animation.hpp"
#include "kernel/presentation/overlay_raster.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace grab::kernel::presentation
{
    namespace
    {

        constexpr std::uint32_t bgraBytesPerPixel      = 4U;
        constexpr std::size_t   blueChannelOffset      = 0U;
        constexpr std::size_t   greenChannelOffset     = 1U;
        constexpr std::size_t   redChannelOffset       = 2U;
        constexpr std::size_t   alphaChannelOffset     = 3U;
        constexpr double        channelMaximum         = 255.0;
        constexpr double        fullyTransparent       = 0.0;
        constexpr double        fullyOpaque            = 1.0;
        constexpr double        antiAliasDamageMargin  = 1.0;
        constexpr std::size_t   antiAliasSubscanlines  = 8U;
        constexpr std::size_t   bezierSubdivisionSteps = 32U;
        constexpr std::size_t   ellipseSamples         = 128U;
        constexpr std::size_t   roundHalfCircleSamples = 16U;
        constexpr std::size_t   circleHalves           = 2U;
        constexpr std::size_t roundCircleSamples = roundHalfCircleSamples * circleHalves;
        constexpr double      roundAngularStepRadians =
            std::numbers::pi_v<double> / static_cast<double>( roundHalfCircleSamples );
        constexpr double fullCircleRadians =
            std::numbers::pi_v<double> * static_cast<double>( circleHalves );
        constexpr double minimumSegmentLengthSquared =
            std::numeric_limits<double>::epsilon();

        struct Contour
        {
                std::size_t first{};
                std::size_t count{};
                bool        closed{};
        };

        struct FlatContours
        {
                std::vector<geometry::PointF> points;
                std::vector<Contour>          contours;
        };

        struct BoundsF
        {
                double left{};
                double top{};
                double right{};
                double bottom{};
        };

        struct Crossing
        {
                double x{};
                int    winding_delta{};
        };

        struct RevealMask
        {
                overlay::Axis axis      = overlay::Axis::X;
                overlay::Edge from_edge = overlay::Edge::Min;
                double        boundary{};
                double        fraction{};
        };

        struct TrackedShape
        {
                overlay::ShapeRecord               record;
                std::optional<geometry::Rectangle> bounds;
                double                             opacity{};
                EvaluatedAnimation                 animation;
                std::optional<RevealMask>          reveal;
        };

        [[nodiscard]]
        geometry::PointF
        device_point( const SpacePoint& point ) noexcept
        {
            return geometry::PointF{ .x = point.x, .y = point.y };
        }

        [[nodiscard]]
        bool
        space_points_equal( const SpacePoint& left,
                            const SpacePoint& right ) noexcept
        {
            return left.x == right.x && left.y == right.y && left.space == right.space;
        }

        [[nodiscard]]
        bool
        colors_equal( const overlay::Color& left,
                      const overlay::Color& right ) noexcept
        {
            return left.r ==
                   right.r &&
                   left.g ==
                   right.g &&
                   left.b ==
                   right.b &&
                   left.a == right.a;
        }

        [[nodiscard]]
        bool
        strokes_equal( const std::optional<overlay::StrokeStyle>& left,
                       const std::optional<overlay::StrokeStyle>& right ) noexcept
        {
            if( left.has_value() != right.has_value() )
            {
                return false;
            }
            return !left.has_value() || ( colors_equal( left->color, right->color ) &&
                                          left->width_px == right->width_px );
        }

        [[nodiscard]]
        bool
        fills_equal( const std::optional<overlay::FillStyle>& left,
                     const std::optional<overlay::FillStyle>& right ) noexcept
        {
            if( left.has_value() != right.has_value() )
            {
                return false;
            }
            return !left.has_value() || colors_equal( left->color, right->color );
        }

        [[nodiscard]]
        bool
        path_commands_equal( const overlay::PathCommand& left,
                             const overlay::PathCommand& right ) noexcept
        {
            if( left.index() != right.index() )
            {
                return false;
            }
            if( const auto* move = std::get_if<overlay::MoveTo>( &left ) )
            {
                const auto* other = std::get_if<overlay::MoveTo>( &right );
                return other !=
                       nullptr &&
                       space_points_equal( move->point, other->point );
            }
            if( const auto* line = std::get_if<overlay::LineTo>( &left ) )
            {
                const auto* other = std::get_if<overlay::LineTo>( &right );
                return other !=
                       nullptr &&
                       space_points_equal( line->point, other->point );
            }
            if( const auto* bezier = std::get_if<overlay::BezierTo>( &left ) )
            {
                const auto* other = std::get_if<overlay::BezierTo>( &right );
                if( other == nullptr )
                {
                    return false;
                }
                return std::ranges::equal( bezier->control,
                                           other->control,
                                           space_points_equal );
            }
            return std::holds_alternative<overlay::ClosePath>( right );
        }

        [[nodiscard]]
        bool
        geometries_equal( const overlay::Geometry& left,
                          const overlay::Geometry& right ) noexcept
        {
            if( left.index() != right.index() )
            {
                return false;
            }
            if( const auto* path = std::get_if<overlay::Path>( &left ) )
            {
                const auto* other = std::get_if<overlay::Path>( &right );
                return other !=
                       nullptr &&
                       path->closed ==
                       other->closed &&
                       std::ranges::equal( path->commands,
                                           other->commands,
                                           path_commands_equal );
            }
            if( const auto* rect = std::get_if<overlay::Rect>( &left ) )
            {
                const auto* other = std::get_if<overlay::Rect>( &right );
                if( other == nullptr )
                {
                    return false;
                }
                return rect->bounds.x ==
                       other->bounds.x &&
                       rect->bounds.y ==
                       other->bounds.y &&
                       rect->bounds.w ==
                       other->bounds.w &&
                       rect->bounds.h ==
                       other->bounds.h &&
                       rect->bounds.space == other->bounds.space;
            }
            if( const auto* ellipse = std::get_if<overlay::Ellipse>( &left ) )
            {
                const auto* other = std::get_if<overlay::Ellipse>( &right );
                return other !=
                       nullptr &&
                       space_points_equal( ellipse->center, other->center ) &&
                       ellipse->radius_x ==
                       other->radius_x &&
                       ellipse->radius_y == other->radius_y;
            }
            const auto* polygon       = std::get_if<overlay::Polygon>( &left );
            const auto* other_polygon = std::get_if<overlay::Polygon>( &right );
            return polygon !=
                   nullptr &&
                   other_polygon !=
                   nullptr &&
                   std::ranges::equal( polygon->points,
                                       other_polygon->points,
                                       space_points_equal );
        }

        [[nodiscard]]
        bool
        appearances_equal( const overlay::Shape& left,
                           const overlay::Shape& right ) noexcept
        {
            return geometries_equal( left.geometry, right.geometry ) &&
                   strokes_equal( left.stroke, right.stroke ) &&
                   fills_equal( left.fill, right.fill ) &&
                   left.band ==
                   right.band &&
                   left.z == right.z;
        }

        [[nodiscard]]
        bool
        reveals_equal( const std::optional<EvaluatedReveal>& left,
                       const std::optional<EvaluatedReveal>& right ) noexcept
        {
            if( left.has_value() != right.has_value() )
            {
                return false;
            }
            return !left.has_value() || ( left->axis ==
                                          right->axis &&
                                          left->from_edge ==
                                          right->from_edge &&
                                          left->fraction == right->fraction );
        }

        [[nodiscard]]
        bool
        animated_geometry_equal( const EvaluatedAnimation& left,
                                 const EvaluatedAnimation& right ) noexcept
        {
            return left.scale ==
                   right.scale &&
                   left.translate_x ==
                   right.translate_x &&
                   left.translate_y ==
                   right.translate_y &&
                   reveals_equal( left.reveal, right.reveal );
        }

        [[nodiscard]]
        bool
        reveal_is_hidden( const std::optional<RevealMask>& reveal ) noexcept
        {
            return reveal.has_value() && reveal->fraction <= fullyTransparent;
        }

        [[nodiscard]]
        bool
        reveal_is_partial( const std::optional<RevealMask>& reveal ) noexcept
        {
            return reveal.has_value() &&
                   reveal->fraction >
                   fullyTransparent &&
                   reveal->fraction < fullyOpaque;
        }

        [[nodiscard]]
        bool
        sample_passes_reveal( double                           sample,
                              const std::optional<RevealMask>& reveal ) noexcept
        {
            if( !reveal.has_value() )
            {
                return true;
            }
            const auto& mask = *reveal;
            if( mask.fraction <=
                fullyTransparent ||
                mask.fraction >=
                fullyOpaque ||
                mask.axis != overlay::Axis::Y )
            {
                return true;
            }
            return mask.from_edge == overlay::Edge::Min ? sample < mask.boundary
                                                        : sample >= mask.boundary;
        }

        [[nodiscard]]
        double
        horizontal_reveal_coverage( std::size_t                      pixel,
                                    const std::optional<RevealMask>& reveal ) noexcept
        {
            if( !reveal.has_value() )
            {
                return fullyOpaque;
            }
            const auto& mask = *reveal;
            if( mask.fraction <=
                fullyTransparent ||
                mask.fraction >=
                fullyOpaque ||
                mask.axis != overlay::Axis::X )
            {
                return fullyOpaque;
            }
            const auto left  = static_cast<double>( pixel );
            const auto right = left + fullyOpaque;
            if( mask.from_edge == overlay::Edge::Min )
            {
                return std::clamp( mask.boundary - left, fullyTransparent, fullyOpaque );
            }
            return std::clamp( right - mask.boundary, fullyTransparent, fullyOpaque );
        }

        [[nodiscard]]
        std::size_t
        start_contour( FlatContours&    contours,
                       geometry::PointF point )
        {
            const auto contour_index = contours.contours.size();
            contours.contours.push_back( Contour{
                .first = contours.points.size(),
                .count = 1U,
            } );
            contours.points.push_back( point );
            return contour_index;
        }

        void
        append_point( FlatContours&    contours,
                      std::size_t      contour_index,
                      geometry::PointF point )
        {
            contours.points.push_back( point );
            ++contours.contours.at( contour_index ).count;
        }

        [[nodiscard]]
        geometry::PointF
        first_point( const FlatContours& contours,
                     std::size_t         contour_index )
        {
            return contours.points.at( contours.contours.at( contour_index ).first );
        }

        void
        append_bezier( FlatContours&               contours,
                       std::size_t                 contour_index,
                       geometry::PointF            current,
                       std::span<const SpacePoint> controls )
        {
            geometry::Curve curve;
            curve.control.reserve( controls.size() + 1U );
            curve.control.push_back( current );
            std::ranges::transform( controls,
                                    std::back_inserter( curve.control ),
                                    device_point );
            for( std::size_t step = 1U; step <= bezierSubdivisionSteps; ++step )
            {
                const auto parameter = static_cast<double>( step ) /
                                       static_cast<double>( bezierSubdivisionSteps );
                append_point( contours, contour_index, curve.evaluate( parameter ) );
            }
        }

        [[nodiscard]]
        FlatContours
        flatten_path( const overlay::Path& path )
        {
            FlatContours                    result;
            std::optional<std::size_t>      active_contour;
            std::optional<geometry::PointF> current;
            for( const auto& command : path.commands )
            {
                if( const auto* move = std::get_if<overlay::MoveTo>( &command ) )
                {
                    current        = device_point( move->point );
                    active_contour = start_contour( result, *current );
                    continue;
                }
                if( const auto* line = std::get_if<overlay::LineTo>( &command ) )
                {
                    const auto endpoint = device_point( line->point );
                    if( !active_contour.has_value() )
                    {
                        active_contour =
                            start_contour( result, current.value_or( endpoint ) );
                    }
                    append_point( result, *active_contour, endpoint );
                    current = endpoint;
                    continue;
                }
                if( const auto* bezier = std::get_if<overlay::BezierTo>( &command ) )
                {
                    if( !current.has_value() && !bezier->control.empty() )
                    {
                        current = device_point( bezier->control.front() );
                    }
                    if( !current.has_value() )
                    {
                        continue;
                    }
                    if( !active_contour.has_value() )
                    {
                        active_contour = start_contour( result, *current );
                    }
                    append_bezier( result, *active_contour, *current, bezier->control );
                    current = result.points.back();
                    continue;
                }
                if( active_contour.has_value() )
                {
                    result.contours.at( *active_contour ).closed = true;
                    current = first_point( result, *active_contour );
                    active_contour.reset();
                }
            }
            if( path.closed && !result.contours.empty() )
            {
                result.contours.back().closed = true;
            }
            return result;
        }

        [[nodiscard]]
        FlatContours
        flatten_rect( const overlay::Rect& rect )
        {
            const auto left   = rect.bounds.x;
            const auto top    = rect.bounds.y;
            const auto right  = left + rect.bounds.w;
            const auto bottom = top + rect.bounds.h;
            return FlatContours{
                .points =
                    {
                             geometry::PointF{ .x = left, .y = top },
                             geometry::PointF{ .x = right, .y = top },
                             geometry::PointF{ .x = right, .y = bottom },
                             geometry::PointF{ .x = left, .y = bottom },
                             },
                .contours = { Contour{ .count = 4U, .closed = true } },
            };
        }

        [[nodiscard]]
        FlatContours
        flatten_ellipse( const overlay::Ellipse& ellipse )
        {
            if( ellipse.radius_x <=
                fullyTransparent ||
                ellipse.radius_y <= fullyTransparent )
            {
                return {};
            }
            FlatContours result;
            result.points.reserve( ellipseSamples );
            result.contours.push_back( Contour{
                .count  = ellipseSamples,
                .closed = true,
            } );
            for( std::size_t sample{}; sample < ellipseSamples; ++sample )
            {
                const auto angle = fullCircleRadians *
                                   static_cast<double>( sample ) /
                                   static_cast<double>( ellipseSamples );
                result.points.push_back( geometry::PointF{
                    .x = ellipse.center.x + ( ellipse.radius_x * std::cos( angle ) ),
                    .y = ellipse.center.y + ( ellipse.radius_y * std::sin( angle ) ),
                } );
            }
            return result;
        }

        [[nodiscard]]
        FlatContours
        flatten_polygon( const overlay::Polygon& polygon )
        {
            if( polygon.points.empty() )
            {
                return {};
            }
            FlatContours result;
            result.points.reserve( polygon.points.size() );
            std::ranges::transform( polygon.points,
                                    std::back_inserter( result.points ),
                                    device_point );
            result.contours.push_back( Contour{
                .count  = result.points.size(),
                .closed = true,
            } );
            return result;
        }

        [[nodiscard]]
        FlatContours
        flatten_geometry( const overlay::Geometry& geometry )
        {
            if( const auto* path = std::get_if<overlay::Path>( &geometry ) )
            {
                return flatten_path( *path );
            }
            if( const auto* rect = std::get_if<overlay::Rect>( &geometry ) )
            {
                return flatten_rect( *rect );
            }
            if( const auto* ellipse = std::get_if<overlay::Ellipse>( &geometry ) )
            {
                return flatten_ellipse( *ellipse );
            }
            return flatten_polygon( std::get<overlay::Polygon>( geometry ) );
        }

        [[nodiscard]]
        bool
        segment_has_length( geometry::PointF start,
                            geometry::PointF end ) noexcept
        {
            const auto delta_x = end.x - start.x;
            const auto delta_y = end.y - start.y;
            return ( delta_x * delta_x ) +
                   ( delta_y * delta_y ) > minimumSegmentLengthSquared;
        }

        [[nodiscard]]
        bool
        has_stroke_segment( const FlatContours& geometry )
        {
            for( const auto& contour : geometry.contours )
            {
                if( contour.count < 2U )
                {
                    continue;
                }
                for( std::size_t offset = 1U; offset < contour.count; ++offset )
                {
                    if( segment_has_length(
                            geometry.points.at( contour.first + offset - 1U ),
                            geometry.points.at( contour.first + offset )
                        ) )
                    {
                        return true;
                    }
                }
                if( contour.closed &&
                    segment_has_length(
                        geometry.points.at( contour.first + contour.count - 1U ),
                        geometry.points.at( contour.first )
                    ) )
                {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]]
        bool
        has_fill_contour( const FlatContours& geometry ) noexcept
        {
            return std::ranges::any_of( geometry.contours,
                                        []( const Contour& contour )
                                        {
                                            return contour.count >= 3U;
                                        } );
        }

        void
        append_closed_contour( FlatContours&                     destination,
                               std::span<const geometry::PointF> points )
        {
            if( points.empty() )
            {
                return;
            }
            destination.contours.push_back( Contour{
                .first  = destination.points.size(),
                .count  = points.size(),
                .closed = true,
            } );
            destination.points.insert( destination.points.end(),
                                       points.begin(),
                                       points.end() );
        }

        void
        append_segment_outline( FlatContours&    destination,
                                geometry::PointF start,
                                geometry::PointF end,
                                double           radius )
        {
            const auto delta_x        = end.x - start.x;
            const auto delta_y        = end.y - start.y;
            const auto length_squared = ( delta_x * delta_x ) + ( delta_y * delta_y );
            if( length_squared <= minimumSegmentLengthSquared )
            {
                return;
            }
            const auto       inverse_length = fullyOpaque / std::sqrt( length_squared );
            const auto       normal_x       = -delta_y * inverse_length * radius;
            const auto       normal_y       = delta_x * inverse_length * radius;
            const std::array outline{
                geometry::PointF{.x = start.x + normal_x, .y = start.y + normal_y},
                geometry::PointF{.x = start.x - normal_x, .y = start.y - normal_y},
                geometry::PointF{  .x = end.x - normal_x,   .y = end.y - normal_y},
                geometry::PointF{  .x = end.x + normal_x,   .y = end.y + normal_y},
            };
            append_closed_contour( destination, outline );
        }

        void
        append_round_outline( FlatContours&    destination,
                              geometry::PointF center,
                              double           radius )
        {
            const auto first = destination.points.size();
            destination.points.reserve( first + roundCircleSamples );
            for( std::size_t sample{}; sample < roundCircleSamples; ++sample )
            {
                const auto angle =
                    static_cast<double>( sample ) * roundAngularStepRadians;
                destination.points.push_back( geometry::PointF{
                    .x = center.x + ( radius * std::cos( angle ) ),
                    .y = center.y + ( radius * std::sin( angle ) ),
                } );
            }
            destination.contours.push_back( Contour{
                .first  = first,
                .count  = roundCircleSamples,
                .closed = true,
            } );
        }

        void
        append_contour_stroke( FlatContours&       destination,
                               const FlatContours& geometry,
                               const Contour&      contour,
                               double              radius )
        {
            if( contour.count < 2U )
            {
                return;
            }
            const auto point_at = [&geometry, &contour]( std::size_t offset )
            {
                return geometry.points.at( contour.first + offset );
            };
            bool has_segment{};
            for( std::size_t offset = 1U; offset < contour.count; ++offset )
            {
                const auto start = point_at( offset - 1U );
                const auto end   = point_at( offset );
                if( segment_has_length( start, end ) )
                {
                    append_segment_outline( destination, start, end, radius );
                    has_segment = true;
                }
            }
            if( contour.closed )
            {
                const auto start = point_at( contour.count - 1U );
                const auto end   = point_at( 0U );
                if( segment_has_length( start, end ) )
                {
                    append_segment_outline( destination, start, end, radius );
                    has_segment = true;
                }
            }
            if( !has_segment )
            {
                return;
            }
            for( std::size_t offset{}; offset < contour.count; ++offset )
            {
                append_round_outline( destination, point_at( offset ), radius );
            }
        }

        [[nodiscard]]
        FlatContours
        stroke_outline( const FlatContours& geometry,
                        double              width )
        {
            FlatContours result;
            const auto   radius = width / static_cast<double>( circleHalves );
            for( const auto& contour : geometry.contours )
            {
                append_contour_stroke( result, geometry, contour, radius );
            }
            return result;
        }

        [[nodiscard]]
        std::optional<BoundsF>
        point_bounds( const FlatContours& contours )
        {
            if( contours.points.empty() )
            {
                return std::nullopt;
            }
            BoundsF bounds{
                .left   = contours.points.front().x,
                .top    = contours.points.front().y,
                .right  = contours.points.front().x,
                .bottom = contours.points.front().y,
            };
            for( const auto point : contours.points )
            {
                bounds.left   = std::min( bounds.left, point.x );
                bounds.top    = std::min( bounds.top, point.y );
                bounds.right  = std::max( bounds.right, point.x );
                bounds.bottom = std::max( bounds.bottom, point.y );
            }
            return bounds;
        }

        void
        scale_contours( FlatContours& contours,
                        double        scale )
        {
            if( scale == fullyOpaque )
            {
                return;
            }
            const auto bounds = point_bounds( contours );
            if( !bounds.has_value() )
            {
                return;
            }
            const auto center_x =
                ( bounds->left + bounds->right ) / static_cast<double>( circleHalves );
            const auto center_y =
                ( bounds->top + bounds->bottom ) / static_cast<double>( circleHalves );
            for( auto& point : contours.points )
            {
                point.x = center_x + ( ( point.x - center_x ) * scale );
                point.y = center_y + ( ( point.y - center_y ) * scale );
            }
        }

        void
        translate_contours( FlatContours& contours,
                            double        translate_x,
                            double        translate_y )
        {
            if( translate_x == fullyTransparent && translate_y == fullyTransparent )
            {
                return;
            }
            for( auto& point : contours.points )
            {
                point.x += translate_x;
                point.y += translate_y;
            }
        }

        [[nodiscard]]
        std::optional<RevealMask>
        apply_animation( FlatContours&             contours,
                         const EvaluatedAnimation& animation )
        {
            scale_contours( contours, animation.scale );

            std::optional<RevealMask> mask;
            if( animation.reveal.has_value() )
            {
                const auto bounds = point_bounds( contours );
                if( bounds.has_value() )
                {
                    const auto clipped = reveal_clip(
                        AnimationRect{
                            .x      = bounds->left,
                            .y      = bounds->top,
                            .width  = bounds->right - bounds->left,
                            .height = bounds->bottom - bounds->top,
                        },
                        *animation.reveal
                    );
                    auto boundary = animation.reveal->axis == overlay::Axis::X
                                      ? clipped.x
                                      : clipped.y;
                    if( animation.reveal->from_edge == overlay::Edge::Min )
                    {
                        boundary += animation.reveal->axis == overlay::Axis::X
                                      ? clipped.width
                                      : clipped.height;
                    }
                    mask = RevealMask{
                        .axis      = animation.reveal->axis,
                        .from_edge = animation.reveal->from_edge,
                        .boundary  = boundary,
                        .fraction  = animation.reveal->fraction,
                    };
                }
            }

            translate_contours( contours, animation.translate_x, animation.translate_y );
            if( mask.has_value() )
            {
                mask->boundary += mask->axis == overlay::Axis::X ? animation.translate_x
                                                                 : animation.translate_y;
            }
            return mask;
        }

        [[nodiscard]]
        std::optional<geometry::Rectangle>
        device_bounds( const BoundsF& bounds,
                       double         padding,
                       geometry::Size surface )
        {
            const auto surface_right  = static_cast<double>( surface.width );
            const auto surface_bottom = static_cast<double>( surface.height );
            const auto left           = std::clamp( std::floor( bounds.left - padding ),
                                                    fullyTransparent,
                                                    surface_right );
            const auto top            = std::clamp( std::floor( bounds.top - padding ),
                                                    fullyTransparent,
                                                    surface_bottom );
            const auto right          = std::clamp( std::ceil( bounds.right + padding ),
                                                    fullyTransparent,
                                                    surface_right );
            const auto bottom         = std::clamp( std::ceil( bounds.bottom + padding ),
                                                    fullyTransparent,
                                                    surface_bottom );
            if( right <= left || bottom <= top )
            {
                return std::nullopt;
            }
            return geometry::Rectangle{
                .x      = static_cast<std::int32_t>( left ),
                .y      = static_cast<std::int32_t>( top ),
                .width  = static_cast<std::uint32_t>( right - left ),
                .height = static_cast<std::uint32_t>( bottom - top ),
            };
        }

        [[nodiscard]]
        std::optional<geometry::Rectangle>
        rendered_bounds( const overlay::Shape& shape,
                         const FlatContours&   geometry,
                         double                opacity,
                         geometry::Size        surface )
        {
            const bool fill_visible   = opacity >
                                        fullyTransparent &&
                                        shape.fill.has_value() &&
                                        shape.fill->color.a !=
                                        0U &&
                                        has_fill_contour( geometry );
            const bool stroke_visible = opacity >
                                        fullyTransparent &&
                                        shape.stroke.has_value() &&
                                        shape.stroke->color.a !=
                                        0U &&
                                        std::isfinite( shape.stroke->width_px ) &&
                                        shape.stroke->width_px >
                                        fullyTransparent &&
                                        has_stroke_segment( geometry );
            if( !fill_visible && !stroke_visible )
            {
                return std::nullopt;
            }
            const auto bounds = point_bounds( geometry );
            if( !bounds.has_value() )
            {
                return std::nullopt;
            }
            const auto stroke_padding = stroke_visible
                                          ? static_cast<double>( shape.stroke->width_px )
                                          : fullyTransparent;
            return device_bounds( *bounds,
                                  stroke_padding + antiAliasDamageMargin,
                                  surface );
        }

        [[nodiscard]]
        std::optional<geometry::Rectangle>
        unite( const std::optional<geometry::Rectangle>& left,
               const std::optional<geometry::Rectangle>& right )
        {
            if( !left.has_value() )
            {
                return right;
            }
            if( !right.has_value() )
            {
                return left;
            }
            const auto left_edge = std::min( left->x, right->x );
            const auto top_edge  = std::min( left->y, right->y );
            const auto right_edge =
                std::max( static_cast<std::int64_t>( left->x ) + left->width,
                          static_cast<std::int64_t>( right->x ) + right->width );
            const auto bottom_edge =
                std::max( static_cast<std::int64_t>( left->y ) + left->height,
                          static_cast<std::int64_t>( right->y ) + right->height );
            return geometry::Rectangle{
                .x = left_edge,
                .y = top_edge,
                .width =
                    static_cast<std::uint32_t>( right_edge -
                                                static_cast<std::int64_t>( left_edge ) ),
                .height =
                    static_cast<std::uint32_t>( bottom_edge -
                                                static_cast<std::int64_t>( top_edge ) ),
            };
        }

        [[nodiscard]]
        std::int64_t
        rectangle_right( geometry::Rectangle rectangle ) noexcept
        {
            return static_cast<std::int64_t>( rectangle.x ) +
                   static_cast<std::int64_t>( rectangle.width );
        }

        [[nodiscard]]
        std::int64_t
        rectangle_bottom( geometry::Rectangle rectangle ) noexcept
        {
            return static_cast<std::int64_t>( rectangle.y ) +
                   static_cast<std::int64_t>( rectangle.height );
        }

        [[nodiscard]]
        std::optional<geometry::Rectangle>
        apply_reveal_to_bounds( const std::optional<geometry::Rectangle>& bounds,
                                const std::optional<RevealMask>&          reveal,
                                geometry::Size                            surface )
        {
            if( !bounds.has_value() || !reveal.has_value() )
            {
                return bounds;
            }
            if( reveal_is_hidden( reveal ) )
            {
                return std::nullopt;
            }
            if( !reveal_is_partial( reveal ) )
            {
                return bounds;
            }

            auto left   = static_cast<std::int64_t>( bounds->x );
            auto top    = static_cast<std::int64_t>( bounds->y );
            auto right  = rectangle_right( *bounds );
            auto bottom = rectangle_bottom( *bounds );
            if( reveal->axis == overlay::Axis::X )
            {
                const auto boundary = std::clamp( reveal->boundary,
                                                  fullyTransparent,
                                                  static_cast<double>( surface.width ) );
                if( reveal->from_edge == overlay::Edge::Min )
                {
                    right =
                        std::min( right,
                                  static_cast<std::int64_t>( std::ceil( boundary ) ) );
                }
                else
                {
                    left =
                        std::max( left,
                                  static_cast<std::int64_t>( std::floor( boundary ) ) );
                }
            }
            else
            {
                const auto boundary =
                    std::clamp( reveal->boundary,
                                fullyTransparent,
                                static_cast<double>( surface.height ) );
                if( reveal->from_edge == overlay::Edge::Min )
                {
                    bottom =
                        std::min( bottom,
                                  static_cast<std::int64_t>( std::ceil( boundary ) ) );
                }
                else
                {
                    top =
                        std::max( top,
                                  static_cast<std::int64_t>( std::floor( boundary ) ) );
                }
            }

            if( right <= left || bottom <= top )
            {
                return std::nullopt;
            }
            return geometry::Rectangle{
                .x      = static_cast<std::int32_t>( left ),
                .y      = static_cast<std::int32_t>( top ),
                .width  = static_cast<std::uint32_t>( right - left ),
                .height = static_cast<std::uint32_t>( bottom - top ),
            };
        }

        [[nodiscard]]
        std::optional<geometry::Rectangle>
        intersection( geometry::Rectangle left,
                      geometry::Rectangle right ) noexcept
        {
            const auto left_edge = std::max( static_cast<std::int64_t>( left.x ),
                                             static_cast<std::int64_t>( right.x ) );
            const auto top_edge  = std::max( static_cast<std::int64_t>( left.y ),
                                             static_cast<std::int64_t>( right.y ) );
            const auto right_edge =
                std::min( rectangle_right( left ), rectangle_right( right ) );
            const auto bottom_edge =
                std::min( rectangle_bottom( left ), rectangle_bottom( right ) );
            if( right_edge <= left_edge || bottom_edge <= top_edge )
            {
                return std::nullopt;
            }
            return geometry::Rectangle{
                .x      = static_cast<std::int32_t>( left_edge ),
                .y      = static_cast<std::int32_t>( top_edge ),
                .width  = static_cast<std::uint32_t>( right_edge - left_edge ),
                .height = static_cast<std::uint32_t>( bottom_edge - top_edge ),
            };
        }

        [[nodiscard]]
        bool
        intersects( geometry::Rectangle left,
                    geometry::Rectangle right ) noexcept
        {
            const auto left_edge = std::max( static_cast<std::int64_t>( left.x ),
                                             static_cast<std::int64_t>( right.x ) );
            const auto top_edge  = std::max( static_cast<std::int64_t>( left.y ),
                                             static_cast<std::int64_t>( right.y ) );
            return left_edge <
                   std::min( rectangle_right( left ), rectangle_right( right ) ) &&
                   top_edge <
                   std::min( rectangle_bottom( left ), rectangle_bottom( right ) );
        }

        void
        append_rectangle( std::vector<geometry::Rectangle>& rectangles,
                          std::int64_t                      left,
                          std::int64_t                      top,
                          std::int64_t                      right,
                          std::int64_t                      bottom )
        {
            if( right <= left || bottom <= top )
            {
                return;
            }
            rectangles.push_back( geometry::Rectangle{
                .x      = static_cast<std::int32_t>( left ),
                .y      = static_cast<std::int32_t>( top ),
                .width  = static_cast<std::uint32_t>( right - left ),
                .height = static_cast<std::uint32_t>( bottom - top ),
            } );
        }

        void
        subtract_rectangle( geometry::Rectangle               rectangle,
                            geometry::Rectangle               covered,
                            std::vector<geometry::Rectangle>& remainder )
        {
            const auto overlap = intersection( rectangle, covered );
            if( !overlap.has_value() )
            {
                remainder.push_back( rectangle );
                return;
            }

            const auto left           = static_cast<std::int64_t>( rectangle.x );
            const auto top            = static_cast<std::int64_t>( rectangle.y );
            const auto right          = rectangle_right( rectangle );
            const auto bottom         = rectangle_bottom( rectangle );
            const auto overlap_left   = static_cast<std::int64_t>( overlap->x );
            const auto overlap_top    = static_cast<std::int64_t>( overlap->y );
            const auto overlap_right  = rectangle_right( *overlap );
            const auto overlap_bottom = rectangle_bottom( *overlap );

            append_rectangle( remainder, left, top, right, overlap_top );
            append_rectangle( remainder, left, overlap_bottom, right, bottom );
            append_rectangle( remainder,
                              left,
                              overlap_top,
                              overlap_left,
                              overlap_bottom );
            append_rectangle( remainder,
                              overlap_right,
                              overlap_top,
                              right,
                              overlap_bottom );
        }

        [[nodiscard]]
        std::vector<geometry::Rectangle>
        normalize_damage( std::span<const geometry::Rectangle> damage )
        {
            std::vector<geometry::Rectangle> normalized;
            std::vector<geometry::Rectangle> pending;
            std::vector<geometry::Rectangle> remainder;
            normalized.reserve( damage.size() );
            for( const auto rectangle : damage )
            {
                pending.clear();
                pending.push_back( rectangle );
                for( const auto covered : normalized )
                {
                    remainder.clear();
                    for( const auto piece : pending )
                    {
                        subtract_rectangle( piece, covered, remainder );
                    }
                    pending.swap( remainder );
                    if( pending.empty() )
                    {
                        break;
                    }
                }
                normalized.insert( normalized.end(),
                                   std::make_move_iterator( pending.begin() ),
                                   std::make_move_iterator( pending.end() ) );
            }
            return normalized;
        }

        [[nodiscard]]
        bool
        intersects_damage( geometry::Rectangle                  bounds,
                           std::span<const geometry::Rectangle> damage ) noexcept
        {
            return std::ranges::any_of( damage,
                                        [bounds]( geometry::Rectangle rectangle )
                                        {
                                            return intersects( bounds, rectangle );
                                        } );
        }

        void
        clear_damage( Image&                               image,
                      std::span<const geometry::Rectangle> damage )
        {
            auto pixels = std::span<std::byte>{ image.pixels };
            for( const auto rectangle : damage )
            {
                if( rectangle.width == 0U || rectangle.height == 0U )
                {
                    continue;
                }
                assert( rectangle.x >= 0 );
                assert( rectangle.y >= 0 );
                assert( rectangle_right( rectangle ) <=
                        static_cast<std::int64_t>( image.width ) );
                assert( rectangle_bottom( rectangle ) <=
                        static_cast<std::int64_t>( image.height ) );
                const auto first_x = static_cast<std::size_t>( rectangle.x );
                const auto first_y = static_cast<std::size_t>( rectangle.y );
                const auto row_bytes =
                    static_cast<std::size_t>( rectangle.width ) * bgraBytesPerPixel;
                for( std::size_t row{}; row < rectangle.height; ++row )
                {
                    const auto offset = ( ( first_y + row ) *
                                          static_cast<std::size_t>( image.stride ) ) +
                                        ( first_x * bgraBytesPerPixel );
                    std::ranges::fill( pixels.subspan( offset, row_bytes ),
                                       std::byte{} );
                }
            }
        }

        [[nodiscard]]
        std::uint8_t
        channel_byte( double value )
        {
            const auto clamped = std::clamp( value, fullyTransparent, channelMaximum );
            return static_cast<std::uint8_t>( std::lround( clamped ) );
        }

        void
        blend_pixel( Image&                image,
                     std::size_t           x,
                     std::size_t           y,
                     const overlay::Color& color,
                     double                opacity,
                     double                coverage )
        {
            const auto source_alpha =
                ( static_cast<double>( color.a ) / channelMaximum ) * opacity * coverage;
            if( source_alpha <= fullyTransparent )
            {
                return;
            }
            const auto inverse_alpha = fullyOpaque - source_alpha;
            const auto pixel_offset  = ( y * static_cast<std::size_t>( image.stride ) ) +
                                       ( x * bgraBytesPerPixel );
            const auto destination   = [&image, pixel_offset]( std::size_t channel )
            {
                return static_cast<double>( std::to_integer<std::uint8_t>(
                    image.pixels.at( pixel_offset + channel )
                ) );
            };
            image.pixels.at( pixel_offset + blueChannelOffset ) = std::byte{
                channel_byte( ( static_cast<double>( color.b ) * source_alpha ) +
                              ( destination( blueChannelOffset ) * inverse_alpha ) ),
            };
            image.pixels.at( pixel_offset + greenChannelOffset ) = std::byte{
                channel_byte( ( static_cast<double>( color.g ) * source_alpha ) +
                              ( destination( greenChannelOffset ) * inverse_alpha ) ),
            };
            image.pixels.at( pixel_offset + redChannelOffset ) = std::byte{
                channel_byte( ( static_cast<double>( color.r ) * source_alpha ) +
                              ( destination( redChannelOffset ) * inverse_alpha ) ),
            };
            image.pixels.at( pixel_offset + alphaChannelOffset ) = std::byte{
                channel_byte( ( channelMaximum * source_alpha ) +
                              ( destination( alphaChannelOffset ) * inverse_alpha ) ),
            };
        }

        void
        append_crossings( const FlatContours&    contours,
                          double                 sample_y,
                          std::vector<Crossing>& crossings )
        {
            for( const auto& contour : contours.contours )
            {
                if( contour.count < 2U )
                {
                    continue;
                }
                for( std::size_t offset{}; offset < contour.count; ++offset )
                {
                    const auto next_offset = ( offset + 1U ) % contour.count;
                    const auto start = contours.points.at( contour.first + offset );
                    const auto end   = contours.points.at( contour.first + next_offset );
                    const bool downward = start.y <= sample_y && sample_y < end.y;
                    const bool upward   = end.y <= sample_y && sample_y < start.y;
                    if( !downward && !upward )
                    {
                        continue;
                    }
                    const auto x = start.x + ( ( sample_y - start.y ) *
                                               ( end.x - start.x ) /
                                               ( end.y - start.y ) );
                    crossings.push_back( Crossing{
                        .x             = x,
                        .winding_delta = downward ? 1 : -1,
                    } );
                }
            }
            std::ranges::sort( crossings, {}, &Crossing::x );
        }

        void
        accumulate_interval( std::vector<double>& coverage,
                             double               interval_start,
                             double               interval_end,
                             std::size_t          first_pixel,
                             std::size_t          last_pixel )
        {
            const auto clipped_start =
                std::max( interval_start, static_cast<double>( first_pixel ) );
            const auto clipped_end =
                std::min( interval_end, static_cast<double>( last_pixel ) );
            if( clipped_end <= clipped_start )
            {
                return;
            }
            const auto first = static_cast<std::size_t>( std::floor( clipped_start ) );
            const auto last  = static_cast<std::size_t>( std::ceil( clipped_end ) );
            const auto sample_weight =
                fullyOpaque / static_cast<double>( antiAliasSubscanlines );
            for( auto pixel = first; pixel < last; ++pixel )
            {
                const auto pixel_start = static_cast<double>( pixel );
                const auto overlap = std::min( clipped_end, pixel_start + fullyOpaque ) -
                                     std::max( clipped_start, pixel_start );
                if( overlap > fullyTransparent )
                {
                    coverage.at( pixel ) += overlap * sample_weight;
                }
            }
        }

        void
        accumulate_scanline( const FlatContours&    contours,
                             double                 sample_y,
                             std::vector<double>&   coverage,
                             std::size_t            first_pixel,
                             std::size_t            last_pixel,
                             std::vector<Crossing>& crossings )
        {
            crossings.clear();
            append_crossings( contours, sample_y, crossings );
            int    winding{};
            double previous_x{};
            bool   has_previous{};
            for( const auto crossing : crossings )
            {
                if( has_previous && winding != 0 )
                {
                    accumulate_interval( coverage,
                                         previous_x,
                                         crossing.x,
                                         first_pixel,
                                         last_pixel );
                }
                winding      += crossing.winding_delta;
                previous_x    = crossing.x;
                has_previous  = true;
            }
        }

        [[nodiscard]]
        std::size_t
        clipped_lower_pixel( double        value,
                             std::uint32_t limit )
        {
            const auto clipped = std::clamp( std::floor( value ),
                                             fullyTransparent,
                                             static_cast<double>( limit ) );
            return static_cast<std::size_t>( clipped );
        }

        [[nodiscard]]
        std::size_t
        clipped_upper_pixel( double        value,
                             std::uint32_t limit )
        {
            const auto clipped = std::clamp( std::ceil( value ),
                                             fullyTransparent,
                                             static_cast<double>( limit ) );
            return static_cast<std::size_t>( clipped );
        }

        void
        rasterize_contours( const FlatContours&                  contours,
                            const overlay::Color&                color,
                            double                               opacity,
                            const std::optional<RevealMask>&     reveal,
                            std::span<const geometry::Rectangle> damage,
                            Image&                               image,
                            std::vector<double>&                 row_coverage,
                            std::vector<Crossing>&               crossings )
        {
            const auto bounds = point_bounds( contours );
            if( !bounds.has_value() ||
                color.a ==
                0U ||
                opacity <=
                fullyTransparent ||
                reveal_is_hidden( reveal ) )
            {
                return;
            }
            auto first_x = clipped_lower_pixel( bounds->left, image.width );
            auto last_x  = clipped_upper_pixel( bounds->right, image.width );
            auto first_y = clipped_lower_pixel( bounds->top, image.height );
            auto last_y  = clipped_upper_pixel( bounds->bottom, image.height );
            if( reveal.has_value() )
            {
                const auto& mask = *reveal;
                const auto  partial =
                    mask.fraction > fullyTransparent && mask.fraction < fullyOpaque;
                if( partial && mask.axis == overlay::Axis::X )
                {
                    if( mask.from_edge == overlay::Edge::Min )
                    {
                        last_x = std::min( last_x,
                                           clipped_upper_pixel( mask.boundary,
                                                                image.width ) );
                    }
                    else
                    {
                        first_x = std::max( first_x,
                                            clipped_lower_pixel( mask.boundary,
                                                                 image.width ) );
                    }
                }
                else if( partial && mask.from_edge == overlay::Edge::Min )
                {
                    last_y =
                        std::min( last_y,
                                  clipped_upper_pixel( mask.boundary, image.height ) );
                }
                else if( partial )
                {
                    first_y =
                        std::max( first_y,
                                  clipped_lower_pixel( mask.boundary, image.height ) );
                }
            }
            if( first_x >= last_x || first_y >= last_y )
            {
                return;
            }
            for( const auto clip : damage )
            {
                assert( clip.x >= 0 );
                assert( clip.y >= 0 );
                const auto clipped_first_x =
                    std::max( first_x, static_cast<std::size_t>( clip.x ) );
                const auto clipped_last_x =
                    std::min( last_x,
                              static_cast<std::size_t>( rectangle_right( clip ) ) );
                const auto clipped_first_y =
                    std::max( first_y, static_cast<std::size_t>( clip.y ) );
                const auto clipped_last_y =
                    std::min( last_y,
                              static_cast<std::size_t>( rectangle_bottom( clip ) ) );
                if( clipped_first_x >=
                    clipped_last_x ||
                    clipped_first_y >= clipped_last_y )
                {
                    continue;
                }
                for( auto y = clipped_first_y; y < clipped_last_y; ++y )
                {
                    std::ranges::fill(
                        row_coverage.begin() +
                            static_cast<std::ptrdiff_t>( clipped_first_x ),
                        row_coverage.begin() +
                            static_cast<std::ptrdiff_t>( clipped_last_x ),
                        fullyTransparent
                    );
                    for( std::size_t sample{}; sample < antiAliasSubscanlines; ++sample )
                    {
                        const auto sample_y =
                            static_cast<double>( y ) +
                            ( ( static_cast<double>( sample ) +
                                ( fullyOpaque / static_cast<double>( circleHalves ) ) ) /
                              static_cast<double>( antiAliasSubscanlines ) );
                        if( !sample_passes_reveal( sample_y, reveal ) )
                        {
                            continue;
                        }
                        accumulate_scanline( contours,
                                             sample_y,
                                             row_coverage,
                                             clipped_first_x,
                                             clipped_last_x,
                                             crossings );
                    }
                    for( auto x = clipped_first_x; x < clipped_last_x; ++x )
                    {
                        const auto coverage = std::clamp( row_coverage.at( x ),
                                                          fullyTransparent,
                                                          fullyOpaque ) *
                                              horizontal_reveal_coverage( x, reveal );
                        blend_pixel( image, x, y, color, opacity, coverage );
                    }
                }
            }
        }

        void
        paint_shapes( std::span<const TrackedShape>        shapes,
                      std::span<const FlatContours>        geometries,
                      std::span<const geometry::Rectangle> damage,
                      Image&                               image,
                      std::vector<double>&                 row_coverage,
                      std::vector<Crossing>&               crossings )
        {
            assert( shapes.size() == geometries.size() );
            auto geometry_iterator = geometries.begin();
            for( const auto& tracked : shapes )
            {
                const auto& geometry = *geometry_iterator;
                ++geometry_iterator;
                if( !tracked.bounds.has_value() ||
                    !intersects_damage( *tracked.bounds, damage ) )
                {
                    continue;
                }
                if( tracked.record.shape.fill.has_value() )
                {
                    rasterize_contours( geometry,
                                        tracked.record.shape.fill->color,
                                        tracked.opacity,
                                        tracked.reveal,
                                        damage,
                                        image,
                                        row_coverage,
                                        crossings );
                }
                if( tracked.record.shape.stroke.has_value() &&
                    std::isfinite( tracked.record.shape.stroke->width_px ) &&
                    tracked.record.shape.stroke->width_px > fullyTransparent )
                {
                    const auto outline = stroke_outline(
                        geometry,
                        static_cast<double>( tracked.record.shape.stroke->width_px )
                    );
                    rasterize_contours( outline,
                                        tracked.record.shape.stroke->color,
                                        tracked.opacity,
                                        tracked.reveal,
                                        damage,
                                        image,
                                        row_coverage,
                                        crossings );
                }
            }
        }

        [[nodiscard,
          maybe_unused]]
        bool
        record_order_less( const overlay::ShapeRecord& left,
                           const overlay::ShapeRecord& right ) noexcept
        {
            return std::tie( left.shape.band, left.shape.z, left.id.slot ) <
                   std::tie( right.shape.band, right.shape.z, right.id.slot );
        }

    }    // namespace

    struct OverlayRaster::Impl
    {
            explicit Impl( geometry::Size raster_size,
                           std::uint32_t  raster_stride,
                           std::size_t    pixel_bytes ) :
                image{
                    .width  = raster_size.width,
                    .height = raster_size.height,
                    .stride = raster_stride,
                    .format = PixelFormat::Bgra,
                    .pixels = std::vector<std::byte>( pixel_bytes ),
                },
                row_coverage( raster_size.width )
            {
            }

            Image                     image;
            std::vector<double>       row_coverage;
            std::vector<Crossing>     crossings;
            std::vector<TrackedShape> previous_shapes;
            bool                      full_redraw_required{ true };
    };

    OverlayRaster::OverlayRaster( std::unique_ptr<Impl> impl ) noexcept :
        impl_{ std::move( impl ) }
    {
    }

    OverlayRaster::~OverlayRaster()                          = default;

    OverlayRaster::OverlayRaster( OverlayRaster&& ) noexcept = default;

    OverlayRaster&
    OverlayRaster::operator=( OverlayRaster&& ) noexcept = default;

    Result<OverlayRaster>
    OverlayRaster::create( geometry::Size size )
    {
        constexpr auto maximumStride = std::numeric_limits<std::uint32_t>::max();
        if( size.width > maximumStride / bgraBytesPerPixel )
        {
            return fail( ErrorCode::Overflowed,
                         "overlay raster stride exceeds image limits" );
        }
        const auto     stride        = size.width * bgraBytesPerPixel;
        constexpr auto maximumBuffer = std::numeric_limits<std::size_t>::max();
        if( size.height !=
            0U &&
            static_cast<std::size_t>( stride ) >
            maximumBuffer /
            size.height )
        {
            return fail( ErrorCode::Overflowed,
                         "overlay raster pixel buffer size overflows" );
        }
        const auto pixel_bytes = static_cast<std::size_t>( stride ) * size.height;
        try
        {
            return OverlayRaster{
                std::make_unique<Impl>( size, stride, pixel_bytes ),
            };
        }
        catch( const std::bad_alloc& )
        {
            return fail( ErrorCode::Overflowed,
                         "overlay raster pixel buffer allocation failed" );
        }
        catch( const std::length_error& )
        {
            return fail( ErrorCode::Overflowed,
                         "overlay raster pixel buffer exceeds container limits" );
        }
    }

    Result<RasterFrame>
    OverlayRaster::render( std::span<const overlay::ShapeRecord> shapes,
                           std::chrono::milliseconds             now )
    {
        assert( std::ranges::is_sorted( shapes, record_order_less ) );
        try
        {
            std::vector<TrackedShape>        current_shapes;
            std::vector<FlatContours>        flattened_shapes;
            std::vector<geometry::Rectangle> damage;
            current_shapes.reserve( shapes.size() );
            flattened_shapes.reserve( shapes.size() );
            damage.reserve( shapes.size() + impl_->previous_shapes.size() );

            for( const auto& record : shapes )
            {
                const auto animation = evaluate_animation( record, now );
                const auto opacity   = evaluate_opacity( record, now );
                auto       geometry  = flatten_geometry( record.shape.geometry );
                const auto reveal    = apply_animation( geometry, animation );
                const auto bounds =
                    apply_reveal_to_bounds( rendered_bounds( record.shape,
                                                             geometry,
                                                             opacity,
                                                             impl_->image.size() ),
                                            reveal,
                                            impl_->image.size() );
                const auto previous =
                    std::ranges::find( impl_->previous_shapes,
                                       record.id.slot,
                                       []( const TrackedShape& tracked )
                                       {
                                           return tracked.record.id.slot;
                                       } );
                const bool changed =
                    previous ==
                    impl_->previous_shapes.end() ||
                    !appearances_equal( previous->record.shape, record.shape ) ||
                    !animated_geometry_equal( previous->animation, animation ) ||
                    previous->opacity != opacity;
                if( changed )
                {
                    const auto previous_shape_bounds =
                        previous == impl_->previous_shapes.end()
                            ? std::optional<geometry::Rectangle>{}
                            : previous->bounds;
                    const auto changed_bounds = unite( previous_shape_bounds, bounds );
                    if( changed_bounds.has_value() )
                    {
                        damage.push_back( *changed_bounds );
                    }
                }
                current_shapes.push_back( TrackedShape{
                    .record    = record,
                    .bounds    = bounds,
                    .opacity   = opacity,
                    .animation = animation,
                    .reveal    = reveal,
                } );
                flattened_shapes.push_back( std::move( geometry ) );
            }

            for( const auto& previous : impl_->previous_shapes )
            {
                const auto still_present =
                    std::ranges::any_of( current_shapes,
                                         [&previous]( const TrackedShape& current )
                                         {
                                             return current.record.id.slot ==
                                                    previous.record.id.slot;
                                         } );
                if( !still_present && previous.bounds.has_value() )
                {
                    damage.push_back( *previous.bounds );
                }
            }

            if( impl_->full_redraw_required )
            {
                damage = {
                    geometry::Rectangle{
                                        .width  = impl_->image.width,
                                        .height = impl_->image.height,
                                        },
                };
            }

            auto normalized_damage      = normalize_damage( damage );
            impl_->full_redraw_required = true;
            clear_damage( impl_->image, normalized_damage );
            paint_shapes( current_shapes,
                          flattened_shapes,
                          normalized_damage,
                          impl_->image,
                          impl_->row_coverage,
                          impl_->crossings );

            impl_->previous_shapes      = std::move( current_shapes );
            impl_->full_redraw_required = false;
            return RasterFrame{
                .pixels = impl_->image,
                .damage = std::move( normalized_damage ),
            };
        }
        catch( const std::bad_alloc& )
        {
            impl_->full_redraw_required = true;
            return fail( ErrorCode::Overflowed,
                         "overlay raster working memory allocation failed" );
        }
        catch( const std::length_error& )
        {
            impl_->full_redraw_required = true;
            return fail( ErrorCode::Overflowed,
                         "overlay raster working memory exceeds container limits" );
        }
    }

    geometry::Size
    OverlayRaster::size() const noexcept
    {
        if( impl_ == nullptr )
        {
            return {};
        }
        return impl_->image.size();
    }

}    // namespace grab::kernel::presentation
