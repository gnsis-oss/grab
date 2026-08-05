#include "grab/geometry/rectangle.hpp"
#include "grab/overlay.hpp"
#include "grab/space.hpp"
#include "kernel/presentation/overlay_edit.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <numbers>
#include <optional>
#include <ranges>
#include <span>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace grab::kernel::presentation
{
    namespace
    {

        constexpr std::size_t bezierSubdivisionSteps = 32U;
        constexpr std::size_t ellipseSamples         = 128U;
        constexpr std::size_t resizeHandleCount      = 8U;
        constexpr double      fullTurnRadians        = 2.0 * std::numbers::pi_v<double>;
        constexpr double      half                   = 0.5;
        constexpr double      unit                   = 1.0;
        constexpr double      pixelHalfDiagonal = std::numbers::sqrt2_v<double> * half;
        constexpr double minimumLengthSquared   = std::numeric_limits<double>::epsilon();

        struct PointD
        {
                double x{};
                double y{};
        };

        struct Contour
        {
                std::size_t first{};
                std::size_t count{};
                bool        closed{};
        };

        struct FlatGeometry
        {
                std::vector<PointD>  points;
                std::vector<Contour> contours;
        };

        struct Bounds
        {
                double left{};
                double top{};
                double right{};
                double bottom{};
        };

        enum class Handle : std::uint8_t
        {
            TopLeft,
            Top,
            TopRight,
            Right,
            BottomRight,
            Bottom,
            BottomLeft,
            Left,
        };

        enum class AxisDirection : std::int8_t
        {
            Minimum = -1,
            Fixed   = 0,
            Maximum = 1,
        };

        struct HandleLocation
        {
                Handle handle{};
                PointD center{};
        };

        struct Candidate
        {
                const overlay::ShapeRecord* record{};
                FlatGeometry                geometry;
                Bounds                      bounds{};
                bool                        fill_visible{};
                double                      stroke_width{};
        };

        struct Selection
        {
                const overlay::ShapeRecord* record{};
                std::optional<Handle>       handle;
        };

        [[nodiscard]]
        bool
        is_finite( PointD point ) noexcept
        {
            return std::isfinite( point.x ) && std::isfinite( point.y );
        }

        [[nodiscard]]
        bool
        is_finite( SpacePoint point ) noexcept
        {
            return std::isfinite( point.x ) && std::isfinite( point.y );
        }

        [[nodiscard]]
        double
        valid_nonnegative( double value,
                           double fallback ) noexcept
        {
            return std::isfinite( value ) && value >= 0.0 ? value : fallback;
        }

        [[nodiscard]]
        EditGeometryOptions
        normalized_options( const EditGeometryOptions& options ) noexcept
        {
            return EditGeometryOptions{
                .handle_px = valid_nonnegative( options.handle_px, handle_px ),
                .hit_tolerance_px =
                    valid_nonnegative( options.hit_tolerance_px, hit_tolerance_px ),
                .min_size_px = valid_nonnegative( options.min_size_px, min_size_px ),
            };
        }

        [[nodiscard]]
        bool
        is_editable( overlay::ShapeId                  id,
                     std::span<const overlay::ShapeId> editable ) noexcept
        {
            return std::ranges::find( editable, id ) != editable.end();
        }

        [[nodiscard]]
        bool
        path_is_in_space( const overlay::Path& path,
                          CoordinateSpaceId    space ) noexcept
        {
            const auto point_is_in_space = [space]( SpacePoint point )
            {
                return point.space == space;
            };
            return std::ranges::all_of(
                path.commands,
                [&point_is_in_space]( const overlay::PathCommand& command )
                {
                    if( const auto* move = std::get_if<overlay::MoveTo>( &command ) )
                    {
                        return point_is_in_space( move->point );
                    }
                    if( const auto* line = std::get_if<overlay::LineTo>( &command ) )
                    {
                        return point_is_in_space( line->point );
                    }
                    if( const auto* bezier = std::get_if<overlay::BezierTo>( &command ) )
                    {
                        return std::ranges::all_of( bezier->control, point_is_in_space );
                    }
                    return true;
                }
            );
        }

        [[nodiscard]]
        bool
        geometry_is_in_space( const overlay::Geometry& geometry,
                              CoordinateSpaceId        space ) noexcept
        {
            if( const auto* path = std::get_if<overlay::Path>( &geometry ) )
            {
                return path_is_in_space( *path, space );
            }
            if( const auto* rect = std::get_if<overlay::Rect>( &geometry ) )
            {
                return rect->bounds.space == space;
            }
            if( const auto* ellipse = std::get_if<overlay::Ellipse>( &geometry ) )
            {
                return ellipse->center.space == space;
            }
            const auto* polygon = std::get_if<overlay::Polygon>( &geometry );
            return polygon !=
                   nullptr &&
                   std::ranges::all_of( polygon->points,
                                        [space]( SpacePoint point )
                                        {
                                            return point.space == space;
                                        } );
        }

        [[nodiscard]]
        PointD
        to_point( SpacePoint point ) noexcept
        {
            return PointD{ .x = point.x, .y = point.y };
        }

        [[nodiscard]]
        PointD
        lerp( PointD left,
              PointD right,
              double parameter ) noexcept
        {
            return PointD{
                .x = left.x + ( ( right.x - left.x ) * parameter ),
                .y = left.y + ( ( right.y - left.y ) * parameter ),
            };
        }

        [[nodiscard]]
        PointD
        evaluate_bezier( PointD                      start,
                         std::span<const SpacePoint> controls,
                         double                      parameter )
        {
            std::vector<PointD> work;
            work.reserve( controls.size() + 1U );
            work.push_back( start );
            std::ranges::transform( controls, std::back_inserter( work ), to_point );
            for( auto active = work.size(); active > 1U; --active )
            {
                for( std::size_t index{}; index + 1U < active; ++index )
                {
                    work.at( index ) =
                        lerp( work.at( index ), work.at( index + 1U ), parameter );
                }
            }
            return work.front();
        }

        [[nodiscard]]
        std::size_t
        start_contour( FlatGeometry& geometry,
                       PointD        point )
        {
            const auto index = geometry.contours.size();
            geometry.contours.push_back( Contour{
                .first = geometry.points.size(),
                .count = 1U,
            } );
            geometry.points.push_back( point );
            return index;
        }

        void
        append_point( FlatGeometry& geometry,
                      std::size_t   contour,
                      PointD        point )
        {
            geometry.points.push_back( point );
            ++geometry.contours.at( contour ).count;
        }

        [[nodiscard]]
        PointD
        first_point( const FlatGeometry& geometry,
                     std::size_t         contour )
        {
            return geometry.points.at( geometry.contours.at( contour ).first );
        }

        void
        append_bezier( FlatGeometry&               geometry,
                       std::size_t                 contour,
                       PointD                      start,
                       std::span<const SpacePoint> controls )
        {
            for( std::size_t step = 1U; step <= bezierSubdivisionSteps; ++step )
            {
                const auto parameter = static_cast<double>( step ) /
                                       static_cast<double>( bezierSubdivisionSteps );
                append_point( geometry,
                              contour,
                              evaluate_bezier( start, controls, parameter ) );
            }
        }

        [[nodiscard]]
        FlatGeometry
        flatten_path( const overlay::Path& path )
        {
            FlatGeometry               result;
            std::optional<std::size_t> active_contour;
            std::optional<PointD>      current;
            for( const auto& command : path.commands )
            {
                if( const auto* move = std::get_if<overlay::MoveTo>( &command ) )
                {
                    current        = to_point( move->point );
                    active_contour = start_contour( result, *current );
                    continue;
                }
                if( const auto* line = std::get_if<overlay::LineTo>( &command ) )
                {
                    const auto endpoint = to_point( line->point );
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
                        current = to_point( bezier->control.front() );
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
        FlatGeometry
        flatten_rect( const overlay::Rect& rect )
        {
            const auto left   = rect.bounds.x;
            const auto top    = rect.bounds.y;
            const auto right  = left + rect.bounds.w;
            const auto bottom = top + rect.bounds.h;
            return FlatGeometry{
                .points =
                    {
                             PointD{ .x = left, .y = top },
                             PointD{ .x = right, .y = top },
                             PointD{ .x = right, .y = bottom },
                             PointD{ .x = left, .y = bottom },
                             },
                .contours = { Contour{ .count = 4U, .closed = true } },
            };
        }

        [[nodiscard]]
        FlatGeometry
        flatten_ellipse( const overlay::Ellipse& ellipse )
        {
            if( ellipse.radius_x <= 0.0 || ellipse.radius_y <= 0.0 )
            {
                return {};
            }
            FlatGeometry result;
            result.points.reserve( ellipseSamples );
            result.contours.push_back( Contour{
                .count  = ellipseSamples,
                .closed = true,
            } );
            for( std::size_t sample{}; sample < ellipseSamples; ++sample )
            {
                const auto angle = fullTurnRadians *
                                   static_cast<double>( sample ) /
                                   static_cast<double>( ellipseSamples );
                result.points.push_back( PointD{
                    .x = ellipse.center.x + ( ellipse.radius_x * std::cos( angle ) ),
                    .y = ellipse.center.y + ( ellipse.radius_y * std::sin( angle ) ),
                } );
            }
            return result;
        }

        [[nodiscard]]
        FlatGeometry
        flatten_polygon( const overlay::Polygon& polygon )
        {
            if( polygon.points.empty() )
            {
                return {};
            }
            FlatGeometry result;
            result.points.reserve( polygon.points.size() );
            std::ranges::transform( polygon.points,
                                    std::back_inserter( result.points ),
                                    to_point );
            result.contours.push_back( Contour{
                .count  = result.points.size(),
                .closed = true,
            } );
            return result;
        }

        [[nodiscard]]
        FlatGeometry
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
            const auto* polygon = std::get_if<overlay::Polygon>( &geometry );
            return polygon == nullptr ? FlatGeometry{} : flatten_polygon( *polygon );
        }

        [[nodiscard]]
        std::optional<Bounds>
        point_bounds( const FlatGeometry& geometry ) noexcept
        {
            if( geometry.points.empty() )
            {
                return std::nullopt;
            }
            Bounds result{
                .left   = geometry.points.front().x,
                .top    = geometry.points.front().y,
                .right  = geometry.points.front().x,
                .bottom = geometry.points.front().y,
            };
            for( const auto point : geometry.points )
            {
                result.left   = std::min( result.left, point.x );
                result.top    = std::min( result.top, point.y );
                result.right  = std::max( result.right, point.x );
                result.bottom = std::max( result.bottom, point.y );
            }
            return result;
        }

        [[nodiscard]]
        bool
        bounds_are_finite( const Bounds& bounds ) noexcept
        {
            return std::isfinite( bounds.left ) &&
                   std::isfinite( bounds.top ) &&
                   std::isfinite( bounds.right ) &&
                   std::isfinite( bounds.bottom ) &&
                   bounds.left <=
                   bounds.right &&
                   bounds.top <= bounds.bottom;
        }

        [[nodiscard]]
        std::optional<Bounds>
        geometry_bounds( const overlay::Geometry& geometry )
        {
            if( const auto* rect = std::get_if<overlay::Rect>( &geometry ) )
            {
                const auto other_x = rect->bounds.x + rect->bounds.w;
                const auto other_y = rect->bounds.y + rect->bounds.h;
                return Bounds{
                    .left   = std::min( rect->bounds.x, other_x ),
                    .top    = std::min( rect->bounds.y, other_y ),
                    .right  = std::max( rect->bounds.x, other_x ),
                    .bottom = std::max( rect->bounds.y, other_y ),
                };
            }
            if( const auto* ellipse = std::get_if<overlay::Ellipse>( &geometry ) )
            {
                return Bounds{
                    .left   = ellipse->center.x - ellipse->radius_x,
                    .top    = ellipse->center.y - ellipse->radius_y,
                    .right  = ellipse->center.x + ellipse->radius_x,
                    .bottom = ellipse->center.y + ellipse->radius_y,
                };
            }
            return point_bounds( flatten_geometry( geometry ) );
        }

        [[nodiscard]]
        std::array<HandleLocation,
                   resizeHandleCount>
        handle_locations( const Bounds& bounds ) noexcept
        {
            const auto center_x = ( bounds.left + bounds.right ) * half;
            const auto center_y = ( bounds.top + bounds.bottom ) * half;
            return std::array{
                HandleLocation{
                               .handle = Handle::TopLeft,
                               .center = PointD{ .x = bounds.left, .y = bounds.top },
                               },
                HandleLocation{
                               .handle = Handle::Top,
                               .center = PointD{ .x = center_x, .y = bounds.top },
                               },
                HandleLocation{
                               .handle = Handle::TopRight,
                               .center = PointD{ .x = bounds.right, .y = bounds.top },
                               },
                HandleLocation{
                               .handle = Handle::Right,
                               .center = PointD{ .x = bounds.right, .y = center_y },
                               },
                HandleLocation{
                               .handle = Handle::BottomRight,
                               .center = PointD{ .x = bounds.right, .y = bounds.bottom },
                               },
                HandleLocation{
                               .handle = Handle::Bottom,
                               .center = PointD{ .x = center_x, .y = bounds.bottom },
                               },
                HandleLocation{
                               .handle = Handle::BottomLeft,
                               .center = PointD{ .x = bounds.left, .y = bounds.bottom },
                               },
                HandleLocation{
                               .handle = Handle::Left,
                               .center = PointD{ .x = bounds.left, .y = center_y },
                               },
            };
        }

        [[nodiscard]]
        std::optional<Handle>
        handle_hit( const Bounds& bounds,
                    PointD        at,
                    double        side ) noexcept
        {
            if( side <= 0.0 )
            {
                return std::nullopt;
            }
            const auto            radius = side * half;
            std::optional<Handle> nearest;
            double                nearest_distance{};
            for( const auto& location : handle_locations( bounds ) )
            {
                if( std::abs( at.x - location.center.x ) <=
                    radius &&
                    std::abs( at.y - location.center.y ) <= radius )
                {
                    const auto delta_x  = at.x - location.center.x;
                    const auto delta_y  = at.y - location.center.y;
                    const auto distance = ( delta_x * delta_x ) + ( delta_y * delta_y );
                    if( !nearest.has_value() || distance < nearest_distance )
                    {
                        nearest          = location.handle;
                        nearest_distance = distance;
                    }
                }
            }
            return nearest;
        }

        [[nodiscard]]
        double
        distance_squared_to_segment( PointD point,
                                     PointD start,
                                     PointD end ) noexcept
        {
            const auto segment_x = end.x - start.x;
            const auto segment_y = end.y - start.y;
            const auto length_squared =
                ( segment_x * segment_x ) + ( segment_y * segment_y );
            if( length_squared <= minimumLengthSquared )
            {
                const auto delta_x = point.x - start.x;
                const auto delta_y = point.y - start.y;
                return ( delta_x * delta_x ) + ( delta_y * delta_y );
            }
            const auto parameter =
                std::clamp( ( ( ( point.x - start.x ) * segment_x ) +
                              ( ( point.y - start.y ) * segment_y ) ) /
                                length_squared,
                            0.0,
                            unit );
            const auto nearest = PointD{
                .x = start.x + ( segment_x * parameter ),
                .y = start.y + ( segment_y * parameter ),
            };
            const auto delta_x = point.x - nearest.x;
            const auto delta_y = point.y - nearest.y;
            return ( delta_x * delta_x ) + ( delta_y * delta_y );
        }

        template<typename Function>
        void
        for_each_stroke_segment( const FlatGeometry& geometry,
                                 Function            function )
        {
            for( const auto& contour : geometry.contours )
            {
                if( contour.count < 2U )
                {
                    continue;
                }
                for( std::size_t offset = 1U; offset < contour.count; ++offset )
                {
                    function( geometry.points.at( contour.first + offset - 1U ),
                              geometry.points.at( contour.first + offset ) );
                }
                if( contour.closed )
                {
                    function( geometry.points.at( contour.first + contour.count - 1U ),
                              geometry.points.at( contour.first ) );
                }
            }
        }

        template<typename Function>
        void
        for_each_fill_edge( const FlatGeometry& geometry,
                            Function            function )
        {
            for( const auto& contour : geometry.contours )
            {
                if( contour.count < 2U )
                {
                    continue;
                }
                for( std::size_t offset{}; offset < contour.count; ++offset )
                {
                    const auto next = ( offset + 1U ) % contour.count;
                    function( geometry.points.at( contour.first + offset ),
                              geometry.points.at( contour.first + next ) );
                }
            }
        }

        [[nodiscard]]
        bool
        has_stroke_segment( const FlatGeometry& geometry )
        {
            bool result{};
            for_each_stroke_segment( geometry,
                                     [&result]( PointD start, PointD end )
                                     {
                                         const auto delta_x = end.x - start.x;
                                         const auto delta_y = end.y - start.y;
                                         result = result || ( ( delta_x * delta_x ) +
                                                              ( delta_y * delta_y ) >
                                                              minimumLengthSquared );
                                     } );
            return result;
        }

        [[nodiscard]]
        bool
        has_fill_area( const FlatGeometry& geometry )
        {
            for( const auto& contour : geometry.contours )
            {
                if( contour.count < 3U )
                {
                    continue;
                }
                const auto origin = geometry.points.at( contour.first );
                for( std::size_t first = 1U; first + 1U < contour.count; ++first )
                {
                    const auto left  = geometry.points.at( contour.first + first );
                    const auto right = geometry.points.at( contour.first + first + 1U );
                    const auto cross =
                        ( ( left.x - origin.x ) * ( right.y - origin.y ) ) -
                        ( ( left.y - origin.y ) * ( right.x - origin.x ) );
                    if( std::abs( cross ) > minimumLengthSquared )
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        [[nodiscard]]
        bool
        point_in_fill( const FlatGeometry& geometry,
                       PointD              point )
        {
            int winding{};
            for_each_fill_edge( geometry,
                                [&winding, point]( PointD start, PointD end )
                                {
                                    const bool downward =
                                        start.y <= point.y && point.y < end.y;
                                    const bool upward =
                                        end.y <= point.y && point.y < start.y;
                                    if( !downward && !upward )
                                    {
                                        return;
                                    }
                                    const auto crossing_x =
                                        start.x + ( ( point.y - start.y ) *
                                                    ( end.x - start.x ) /
                                                    ( end.y - start.y ) );
                                    if( crossing_x > point.x )
                                    {
                                        winding += downward ? 1 : -1;
                                    }
                                } );
            return winding != 0;
        }

        [[nodiscard]]
        bool
        point_near_fill( const FlatGeometry& geometry,
                         PointD              point,
                         double              tolerance )
        {
            const auto maximum_distance = tolerance * tolerance;
            bool       near{};
            for_each_fill_edge(
                geometry,
                [&near, point, maximum_distance]( PointD start, PointD end )
                {
                    near = near ||
                           distance_squared_to_segment( point, start, end ) <=
                           maximum_distance;
                }
            );
            return near;
        }

        [[nodiscard]]
        bool
        point_near_stroke( const FlatGeometry& geometry,
                           PointD              point,
                           double              radius )
        {
            const auto maximum_distance = radius * radius;
            bool       near{};
            for_each_stroke_segment(
                geometry,
                [&near, point, maximum_distance]( PointD start, PointD end )
                {
                    near = near ||
                           distance_squared_to_segment( point, start, end ) <=
                           maximum_distance;
                }
            );
            return near;
        }

        [[nodiscard]]
        bool
        fill_is_visible( const overlay::Shape& shape,
                         const FlatGeometry&   geometry )
        {
            return shape.fill.has_value() &&
                   shape.fill->color.a !=
                   0U &&
                   has_fill_area( geometry );
        }

        [[nodiscard]]
        double
        visible_stroke_width( const overlay::Shape& shape,
                              const FlatGeometry&   geometry )
        {
            if( !shape.stroke.has_value() )
            {
                return 0.0;
            }
            const auto& stroke = shape.stroke.value();
            if( stroke.color.a ==
                0U ||
                !std::isfinite( stroke.width_px ) ||
                stroke.width_px <=
                0.0F ||
                !has_stroke_segment( geometry ) )
            {
                return 0.0;
            }
            return static_cast<double>( stroke.width_px );
        }

        [[nodiscard]]
        bool
        body_hit( const FlatGeometry& geometry,
                  bool                fill_visible,
                  double              stroke_width,
                  PointD              at,
                  double              tolerance )
        {
            if( fill_visible && ( point_in_fill( geometry, at ) ||
                                  point_near_fill( geometry, at, tolerance ) ) )
            {
                return true;
            }
            if( stroke_width <= 0.0 )
            {
                return false;
            }
            const auto stroke_radius = ( stroke_width * half ) + tolerance;
            return point_near_stroke( geometry, at, stroke_radius );
        }

        [[nodiscard]]
        std::vector<Candidate>
        candidates_for_point( std::span<const overlay::ShapeRecord> shapes,
                              std::span<const overlay::ShapeId>     editable,
                              SpacePoint                            at )
        {
            std::vector<Candidate> result;
            result.reserve( shapes.size() );
            for( const auto& record : shapes )
            {
                if( !is_editable( record.id, editable ) ||
                    !geometry_is_in_space( record.shape.geometry, at.space ) )
                {
                    continue;
                }
                const auto bounds = geometry_bounds( record.shape.geometry );
                if( !bounds.has_value() || !bounds_are_finite( *bounds ) )
                {
                    continue;
                }
                auto       geometry     = flatten_geometry( record.shape.geometry );
                const auto fill_visible = fill_is_visible( record.shape, geometry );
                const auto stroke_width = visible_stroke_width( record.shape, geometry );
                result.push_back( Candidate{
                    .record       = &record,
                    .geometry     = std::move( geometry ),
                    .bounds       = *bounds,
                    .fill_visible = fill_visible,
                    .stroke_width = stroke_width,
                } );
            }
            std::ranges::sort( result,
                               []( const Candidate& left, const Candidate& right )
                               {
                                   return std::tie( left.record->shape.band,
                                                    left.record->shape.z,
                                                    left.record->id.slot ) <
                                          std::tie( right.record->shape.band,
                                                    right.record->shape.z,
                                                    right.record->id.slot );
                               } );
            return result;
        }

        [[nodiscard]]
        std::optional<Selection>
        detailed_hit_test( std::span<const overlay::ShapeRecord> shapes,
                           std::span<const overlay::ShapeId>     editable,
                           SpacePoint                            at,
                           const EditGeometryOptions&            raw_options )
        {
            if( !is_finite( at ) )
            {
                return std::nullopt;
            }
            const auto options    = normalized_options( raw_options );
            auto       candidates = candidates_for_point( shapes, editable, at );
            const auto point      = to_point( at );

            // Handles form a global priority tier; paint order breaks ties only
            // within that tier.
            for( const auto& candidate : std::views::reverse( candidates ) )
            {
                const auto handle =
                    handle_hit( candidate.bounds, point, options.handle_px );
                if( handle.has_value() )
                {
                    return Selection{
                        .record = candidate.record,
                        .handle = handle,
                    };
                }
            }
            for( const auto& candidate : std::views::reverse( candidates ) )
            {
                if( body_hit( candidate.geometry,
                              candidate.fill_visible,
                              candidate.stroke_width,
                              point,
                              options.hit_tolerance_px ) )
                {
                    return Selection{
                        .record = candidate.record,
                        .handle = std::nullopt,
                    };
                }
            }
            return std::nullopt;
        }

        [[nodiscard]]
        Bounds
        expanded( Bounds bounds,
                  double amount ) noexcept
        {
            bounds.left   -= amount;
            bounds.top    -= amount;
            bounds.right  += amount;
            bounds.bottom += amount;
            return bounds;
        }

        [[nodiscard]]
        std::optional<Bounds>
        painted_hit_bounds( const Bounds& bounds,
                            bool          fill_visible,
                            double        stroke_width,
                            double        tolerance )
        {
            double expansion{};
            bool   visible{};
            if( fill_visible )
            {
                expansion = tolerance;
                visible   = true;
            }
            if( stroke_width > 0.0 )
            {
                expansion = std::max( expansion, ( stroke_width * half ) + tolerance );
                visible   = true;
            }
            if( !visible )
            {
                return std::nullopt;
            }
            return expanded( bounds, expansion );
        }

        [[nodiscard]]
        std::int64_t
        clamped_edge( double value ) noexcept
        {
            constexpr auto minimum = std::numeric_limits<std::int32_t>::min();
            constexpr auto maximum = std::numeric_limits<std::int32_t>::max();
            return static_cast<std::int64_t>(
                std::clamp( value,
                            static_cast<double>( minimum ),
                            static_cast<double>( maximum ) )
            );
        }

        [[nodiscard]]
        std::optional<geometry::Rectangle>
        rectangle_from_edges( std::int64_t left,
                              std::int64_t top,
                              std::int64_t right,
                              std::int64_t bottom ) noexcept
        {
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
        geometry::Rectangle
        unite( geometry::Rectangle left,
               geometry::Rectangle right ) noexcept
        {
            const auto left_edge = std::min( static_cast<std::int64_t>( left.x ),
                                             static_cast<std::int64_t>( right.x ) );
            const auto top_edge  = std::min( static_cast<std::int64_t>( left.y ),
                                             static_cast<std::int64_t>( right.y ) );
            const auto right_edge =
                std::max( static_cast<std::int64_t>( left.x ) + left.width,
                          static_cast<std::int64_t>( right.x ) + right.width );
            const auto bottom_edge =
                std::max( static_cast<std::int64_t>( left.y ) + left.height,
                          static_cast<std::int64_t>( right.y ) + right.height );
            return rectangle_from_edges( left_edge, top_edge, right_edge, bottom_edge )
                .value_or( geometry::Rectangle{} );
        }

        class RegionAccumulator final
        {
            public:

                void
                add( std::optional<geometry::Rectangle> rectangle )
                {
                    if( !rectangle.has_value() )
                    {
                        return;
                    }
                    if( overflow_.has_value() )
                    {
                        overflow_ = unite( *overflow_, *rectangle );
                        return;
                    }
                    if( rectangles_.size() < max_region_rects )
                    {
                        rectangles_.push_back( *rectangle );
                        return;
                    }
                    overflow_ = unite( rectangles_.back(), *rectangle );
                    rectangles_.pop_back();
                }

                [[nodiscard]]
                bool
                overflowed() const noexcept
                {
                    return overflow_.has_value();
                }

                [[nodiscard]]
                std::vector<geometry::Rectangle>
                take()
                {
                    if( overflow_.has_value() )
                    {
                        rectangles_.push_back( *overflow_ );
                    }
                    return std::move( rectangles_ );
                }

            private:

                std::vector<geometry::Rectangle>   rectangles_;
                std::optional<geometry::Rectangle> overflow_;
        };

        void
        add_body_region( RegionAccumulator&  region,
                         const FlatGeometry& geometry,
                         const Bounds&       geometry_bound,
                         bool                fill_visible,
                         double              stroke_width,
                         double              tolerance )
        {
            const auto sampling_tolerance = tolerance + pixelHalfDiagonal;
            const auto bounds             = painted_hit_bounds( geometry_bound,
                                                                fill_visible,
                                                                stroke_width,
                                                                sampling_tolerance );
            if( !bounds.has_value() )
            {
                return;
            }
            const auto left   = clamped_edge( std::floor( bounds->left ) );
            const auto top    = clamped_edge( std::floor( bounds->top ) );
            const auto right  = clamped_edge( std::ceil( bounds->right ) );
            const auto bottom = clamped_edge( std::ceil( bounds->bottom ) );
            if( region.overflowed() )
            {
                region.add( rectangle_from_edges( left, top, right, bottom ) );
                return;
            }

            const auto row_count = bottom - top;
            for( std::int64_t row_index{}; row_index < row_count; ++row_index )
            {
                const auto offset = row_index / 2;
                const auto y = row_index % 2 == 0 ? top + offset : bottom - offset - 1;
                std::optional<std::int64_t> run_start;
                for( auto x = left; x < right; ++x )
                {
                    const auto hit = body_hit( geometry,
                                               fill_visible,
                                               stroke_width,
                                               PointD{
                                                   .x = static_cast<double>( x ) + half,
                                                   .y = static_cast<double>( y ) + half,
                                               },
                                               sampling_tolerance );
                    if( hit && !run_start.has_value() )
                    {
                        run_start = x;
                    }
                    if( !hit && run_start.has_value() )
                    {
                        region.add( rectangle_from_edges( *run_start, y, x, y + 1 ) );
                        run_start.reset();
                    }
                }
                if( run_start.has_value() )
                {
                    region.add( rectangle_from_edges( *run_start, y, right, y + 1 ) );
                }
                if( region.overflowed() )
                {
                    const auto processed        = row_index + 1;
                    const auto processed_top    = ( processed + 1 ) / 2;
                    const auto processed_bottom = processed / 2;
                    region.add( rectangle_from_edges( left,
                                                      top + processed_top,
                                                      right,
                                                      bottom - processed_bottom ) );
                    return;
                }
            }
        }

        [[nodiscard]]
        std::optional<geometry::Rectangle>
        handle_rectangle( PointD center,
                          double side ) noexcept
        {
            if( side <= 0.0 || !is_finite( center ) )
            {
                return std::nullopt;
            }
            const auto radius = side * half;
            return rectangle_from_edges( clamped_edge( std::floor( center.x - radius ) ),
                                         clamped_edge( std::floor( center.y - radius ) ),
                                         clamped_edge( std::ceil( center.x + radius ) ),
                                         clamped_edge( std::ceil( center.y +
                                                                  radius ) ) );
        }

        [[nodiscard]]
        std::pair<AxisDirection,
                  AxisDirection>
        handle_directions( Handle handle ) noexcept
        {
            switch( handle )
            {
                case Handle::TopLeft :
                    return { AxisDirection::Minimum, AxisDirection::Minimum };
                case Handle::Top :
                    return { AxisDirection::Fixed, AxisDirection::Minimum };
                case Handle::TopRight :
                    return { AxisDirection::Maximum, AxisDirection::Minimum };
                case Handle::Right :
                    return { AxisDirection::Maximum, AxisDirection::Fixed };
                case Handle::BottomRight :
                    return { AxisDirection::Maximum, AxisDirection::Maximum };
                case Handle::Bottom :
                    return { AxisDirection::Fixed, AxisDirection::Maximum };
                case Handle::BottomLeft :
                    return { AxisDirection::Minimum, AxisDirection::Maximum };
                case Handle::Left :
                    return { AxisDirection::Minimum, AxisDirection::Fixed };
            }
            return { AxisDirection::Fixed, AxisDirection::Fixed };
        }

        [[nodiscard]]
        std::pair<double,
                  double>
        resized_axis( double        minimum,
                      double        maximum,
                      double        delta,
                      AxisDirection direction,
                      double        minimum_size ) noexcept
        {
            if( direction == AxisDirection::Fixed )
            {
                return { minimum, maximum };
            }
            if( direction == AxisDirection::Minimum )
            {
                return { std::min( minimum + delta, maximum - minimum_size ), maximum };
            }
            return { minimum, std::max( maximum + delta, minimum + minimum_size ) };
        }

        [[nodiscard]]
        Bounds
        resized_bounds( Bounds        original,
                        double        delta_x,
                        double        delta_y,
                        AxisDirection horizontal,
                        AxisDirection vertical,
                        double        minimum_size ) noexcept
        {
            const auto horizontal_bounds = resized_axis( original.left,
                                                         original.right,
                                                         delta_x,
                                                         horizontal,
                                                         minimum_size );
            const auto vertical_bounds   = resized_axis( original.top,
                                                         original.bottom,
                                                         delta_y,
                                                         vertical,
                                                         minimum_size );
            return Bounds{
                .left   = horizontal_bounds.first,
                .top    = vertical_bounds.first,
                .right  = horizontal_bounds.second,
                .bottom = vertical_bounds.second,
            };
        }

        template<typename Function>
        void
        transform_path( overlay::Path& path,
                        Function       function )
        {
            for( auto& command : path.commands )
            {
                if( auto* move = std::get_if<overlay::MoveTo>( &command ) )
                {
                    function( move->point );
                    continue;
                }
                if( auto* line = std::get_if<overlay::LineTo>( &command ) )
                {
                    function( line->point );
                    continue;
                }
                if( auto* bezier = std::get_if<overlay::BezierTo>( &command ) )
                {
                    std::ranges::for_each( bezier->control, function );
                }
            }
        }

        void
        translate_geometry( overlay::Geometry& geometry,
                            double             delta_x,
                            double             delta_y )
        {
            const auto translate_point = [delta_x, delta_y]( SpacePoint& point )
            {
                point.x += delta_x;
                point.y += delta_y;
            };
            if( auto* path = std::get_if<overlay::Path>( &geometry ) )
            {
                transform_path( *path, translate_point );
                return;
            }
            if( auto* rect = std::get_if<overlay::Rect>( &geometry ) )
            {
                rect->bounds.x += delta_x;
                rect->bounds.y += delta_y;
                return;
            }
            if( auto* ellipse = std::get_if<overlay::Ellipse>( &geometry ) )
            {
                translate_point( ellipse->center );
                return;
            }
            if( auto* polygon = std::get_if<overlay::Polygon>( &geometry ) )
            {
                std::ranges::for_each( polygon->points, translate_point );
            }
        }

        [[nodiscard]]
        double
        remap( double value,
               double old_minimum,
               double old_maximum,
               double new_minimum,
               double new_maximum ) noexcept
        {
            const auto old_extent = old_maximum - old_minimum;
            if( old_extent <= minimumLengthSquared )
            {
                return ( new_minimum + new_maximum ) * half;
            }
            const auto fraction = ( value - old_minimum ) / old_extent;
            return new_minimum + ( fraction * ( new_maximum - new_minimum ) );
        }

        void
        scale_geometry( overlay::Geometry& geometry,
                        Bounds             original,
                        Bounds             resized )
        {
            if( auto* rect = std::get_if<overlay::Rect>( &geometry ) )
            {
                rect->bounds.x = resized.left;
                rect->bounds.y = resized.top;
                rect->bounds.w = resized.right - resized.left;
                rect->bounds.h = resized.bottom - resized.top;
                return;
            }
            if( auto* ellipse = std::get_if<overlay::Ellipse>( &geometry ) )
            {
                ellipse->center.x = ( resized.left + resized.right ) * half;
                ellipse->center.y = ( resized.top + resized.bottom ) * half;
                ellipse->radius_x = ( resized.right - resized.left ) * half;
                ellipse->radius_y = ( resized.bottom - resized.top ) * half;
                return;
            }
            const auto scale_point = [original, resized]( SpacePoint& point )
            {
                point.x = remap( point.x,
                                 original.left,
                                 original.right,
                                 resized.left,
                                 resized.right );
                point.y = remap( point.y,
                                 original.top,
                                 original.bottom,
                                 resized.top,
                                 resized.bottom );
            };
            if( auto* path = std::get_if<overlay::Path>( &geometry ) )
            {
                transform_path( *path, scale_point );
                return;
            }
            if( auto* polygon = std::get_if<overlay::Polygon>( &geometry ) )
            {
                std::ranges::for_each( polygon->points, scale_point );
            }
        }

    }    // namespace

    std::optional<overlay::ShapeId>
    hit_test( std::span<const overlay::ShapeRecord> shapes,
              std::span<const overlay::ShapeId>     editable,
              SpacePoint                            at,
              const EditGeometryOptions&            options )
    {
        const auto selection = detailed_hit_test( shapes, editable, at, options );
        if( !selection.has_value() )
        {
            return std::nullopt;
        }
        return selection->record->id;
    }

    std::vector<geometry::Rectangle>
    edit_input_region( std::span<const overlay::ShapeRecord> shapes,
                       std::span<const overlay::ShapeId>     editable,
                       const EditGeometryOptions&            raw_options )
    {
        const auto        options = normalized_options( raw_options );
        RegionAccumulator region;
        for( const auto& record : shapes )
        {
            if( !is_editable( record.id, editable ) )
            {
                continue;
            }
            const auto bounds = geometry_bounds( record.shape.geometry );
            if( !bounds.has_value() || !bounds_are_finite( *bounds ) )
            {
                continue;
            }
            for( const auto& location : handle_locations( *bounds ) )
            {
                region.add( handle_rectangle( location.center, options.handle_px ) );
            }
            const auto geometry     = flatten_geometry( record.shape.geometry );
            const auto fill_visible = fill_is_visible( record.shape, geometry );
            const auto stroke_width = visible_stroke_width( record.shape, geometry );
            add_body_region( region,
                             geometry,
                             *bounds,
                             fill_visible,
                             stroke_width,
                             options.hit_tolerance_px );
        }

        // Native input-region protocols become expensive with thousands of tiny
        // rectangles. Keep a fixed prefix and collapse every excess piece into
        // one bounding rectangle so the result never exceeds this public cap.
        return region.take();
    }

    bool
    EditInteraction::begin( std::span<const overlay::ShapeRecord> shapes,
                            std::span<const overlay::ShapeId>     editable,
                            SpacePoint                            at,
                            const EditGeometryOptions&            options )
    {
        if( active_ )
        {
            return false;
        }
        const auto selection = detailed_hit_test( shapes, editable, at, options );
        if( !selection.has_value() ||
            !is_editable( selection->record->id, editable ) ||
            selection->record->shape.animation.has_value() )
        {
            return false;
        }

        original_ = selection->record->shape;
        target_   = selection->record->id;
        began_at_ = at;
        options_  = normalized_options( options );
        handle_   = ResizeHandle::None;
        if( selection->handle.has_value() )
        {
            switch( *selection->handle )
            {
                case Handle::TopLeft :
                    handle_ = ResizeHandle::TopLeft;
                    break;
                case Handle::Top :
                    handle_ = ResizeHandle::Top;
                    break;
                case Handle::TopRight :
                    handle_ = ResizeHandle::TopRight;
                    break;
                case Handle::Right :
                    handle_ = ResizeHandle::Right;
                    break;
                case Handle::BottomRight :
                    handle_ = ResizeHandle::BottomRight;
                    break;
                case Handle::Bottom :
                    handle_ = ResizeHandle::Bottom;
                    break;
                case Handle::BottomLeft :
                    handle_ = ResizeHandle::BottomLeft;
                    break;
                case Handle::Left :
                    handle_ = ResizeHandle::Left;
                    break;
            }
        }
        active_ = true;
        return true;
    }

    std::optional<overlay::Shape>
    EditInteraction::update( SpacePoint at )
    {
        if( !active_ || !is_finite( at ) || at.space != began_at_.space )
        {
            return std::nullopt;
        }
        auto       result = original_;
        const auto dx     = at.x - began_at_.x;
        const auto dy     = at.y - began_at_.y;
        if( handle_ == ResizeHandle::None )
        {
            translate_geometry( result.geometry, dx, dy );
            return result;
        }

        Handle handle = Handle::TopLeft;
        switch( handle_ )
        {
            case ResizeHandle::None :
                break;
            case ResizeHandle::TopLeft :
                handle = Handle::TopLeft;
                break;
            case ResizeHandle::Top :
                handle = Handle::Top;
                break;
            case ResizeHandle::TopRight :
                handle = Handle::TopRight;
                break;
            case ResizeHandle::Right :
                handle = Handle::Right;
                break;
            case ResizeHandle::BottomRight :
                handle = Handle::BottomRight;
                break;
            case ResizeHandle::Bottom :
                handle = Handle::Bottom;
                break;
            case ResizeHandle::BottomLeft :
                handle = Handle::BottomLeft;
                break;
            case ResizeHandle::Left :
                handle = Handle::Left;
                break;
        }
        const auto original_bounds = geometry_bounds( original_.geometry );
        if( !original_bounds.has_value() )
        {
            return std::nullopt;
        }
        const auto [horizontal, vertical] = handle_directions( handle );
        const auto bounds                 = resized_bounds( *original_bounds,
                                                            dx,
                                                            dy,
                                                            horizontal,
                                                            vertical,
                                                            options_.min_size_px );
        scale_geometry( result.geometry, *original_bounds, bounds );
        return result;
    }

    std::optional<overlay::Shape>
    EditInteraction::commit( SpacePoint at )
    {
        auto result = update( at );
        if( result.has_value() )
        {
            cancel();
        }
        return result;
    }

    void
    EditInteraction::cancel()
    {
        original_ = overlay::Shape{};
        target_   = overlay::ShapeId{};
        began_at_ = SpacePoint{};
        options_  = EditGeometryOptions{};
        handle_   = ResizeHandle::None;
        active_   = false;
    }

    bool
    EditInteraction::active() const noexcept
    {
        return active_;
    }

    overlay::ShapeId
    EditInteraction::target() const noexcept
    {
        return target_;
    }

}    // namespace grab::kernel::presentation
