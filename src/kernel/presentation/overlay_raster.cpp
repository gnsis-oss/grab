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
#include "kernel/support/diag.hpp"
#include "kernel/support/log.hpp"
#include "kernel/support/log_tags.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <numbers>
#include <optional>
#include <ratio>
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
        constexpr std::size_t   circleHalves           = 2U;
        constexpr double        fullCircleRadians =
            std::numbers::pi_v<double> * static_cast<double>( circleHalves );

        // How far a flattened arc may sit from the curve it stands in for.
        // A fifth of a pixel is below what the 8x sub-scanline coverage can
        // resolve, so the chord count derived from it is the smallest that
        // still costs nothing visually.
        constexpr double      maximumChordDeviationPx = 0.05;
        constexpr std::size_t minimumArcChords        = 8U;
        constexpr std::size_t maximumArcChords        = 256U;
        constexpr double      minimumSegmentLengthSquared =
            std::numeric_limits<double>::epsilon();
        // Round-half-up for an already-clamped non-negative channel value.
        constexpr double roundingBias = 0.5;

        // Chords for a circular arc of `radius`, from the deviation budget:
        // a regular n-gon's worst error is radius * (1 - cos(pi / n)).
        //
        // Round caps and joins used to be a fixed 32 chords whatever the stroke
        // width, and an ellipse a fixed 128 whatever its size. Both are wrong
        // in both directions. A trail is a 3-pixel stroke — a 1.5-pixel radius,
        // where 8 chords are already accurate to a tenth of a pixel — and it
        // pays for a round join at every sample, each chord becoming an edge
        // that is sorted once and tested eight times per row it spans. In the
        // other direction a 1500-pixel ellipse radius at 128 chords is visibly
        // faceted at the extremes.
        [[nodiscard]]
        std::size_t
        arc_chord_count( double radius ) noexcept
        {
            if( !( radius > 0.0 ) )
            {
                return minimumArcChords;
            }
            const auto ratio = maximumChordDeviationPx / radius;
            if( ratio >= 1.0 )
            {
                return minimumArcChords;
            }
            const auto chords = std::numbers::pi_v<double> / std::acos( 1.0 - ratio );
            return std::clamp( static_cast<std::size_t>( std::ceil( chords ) ),
                               minimumArcChords,
                               maximumArcChords );
        }

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

        // Per-phase breakdown of one render.
        //
        // "The overlay is laggy" has two entirely different causes with the
        // same symptom: a fading trail re-rasterizes every live segment every
        // frame (cost scales with shape count), and a large annotation covers
        // millions of pixels (cost scales with area). Only a split like this
        // tells them apart, and the residual after a fix is the only way to
        // know whether the fix was the right one.
        //
        // Populated only when a caller passes one. The per-shape timers cost
        // two clock reads each, which at a thousand live segments is real time
        // inside a 16.7 ms budget, so the null pointer is the "not measuring"
        // state and costs one predictable branch per phase.
        struct RenderProfile
        {
                std::chrono::nanoseconds evaluate{};
                std::chrono::nanoseconds clear{};
                std::chrono::nanoseconds outline{};
                std::chrono::nanoseconds fill{};
                std::size_t              rasterize_calls{};
                std::size_t              covered_pixels{};
        };

        using ProfileClock = std::chrono::steady_clock;

        [[nodiscard]]
        ProfileClock::time_point
        profile_now( const RenderProfile* profile ) noexcept
        {
            return profile == nullptr ? ProfileClock::time_point{} : ProfileClock::now();
        }

        void
        profile_add( RenderProfile*           profile,
                     std::chrono::nanoseconds RenderProfile::* bucket,
                     ProfileClock::time_point                  started ) noexcept
        {
            if( profile != nullptr )
            {
                profile->*bucket += ProfileClock::now() - started;
            }
        }

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
            // The wider radius sets the accuracy the other one inherits.
            const auto   samples =
                arc_chord_count( std::max( ellipse.radius_x, ellipse.radius_y ) );
            result.points.reserve( samples );
            result.contours.push_back( Contour{
                .count  = samples,
                .closed = true,
            } );
            for( std::size_t sample{}; sample < samples; ++sample )
            {
                const auto angle = fullCircleRadians *
                                   static_cast<double>( sample ) /
                                   static_cast<double>( samples );
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
            const auto first  = destination.points.size();
            const auto chords = arc_chord_count( radius );
            const auto step   = fullCircleRadians / static_cast<double>( chords );
            destination.points.reserve( first + chords );
            for( std::size_t sample{}; sample < chords; ++sample )
            {
                const auto angle = static_cast<double>( sample ) * step;
                destination.points.push_back( geometry::PointF{
                    .x = center.x + ( radius * std::cos( angle ) ),
                    .y = center.y + ( radius * std::sin( angle ) ),
                } );
            }
            destination.contours.push_back( Contour{
                .first  = first,
                .count  = chords,
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

        // How much area a merge may waste before it stops paying. Two damage
        // rectangles combine when their union costs no more than this many
        // pixels beyond keeping them apart. 4096 is a 64x64 patch: far cheaper
        // to clear than the per-rectangle bookkeeping it removes, and small
        // enough that a trail travelling diagonally across the screen does not
        // chain into one screen-sized box.
        constexpr std::uint64_t mergeWasteBudget = 4'096U;

        // Past this the per-rectangle work outweighs the pixels it saves and
        // the set collapses to its bounding box.
        constexpr std::size_t   maxDamageRectangles = 64U;

        // How wide a shape's clipped extent has to be before a row is resolved
        // through coverage events rather than a value per pixel.
        //
        // The two representations have opposite failure modes. Events cost
        // O(crossings) per row however wide the shape is, but carry a fixed
        // overhead of two small sorts and a few dozen vector appends — which
        // measured as 12.2 ms of fill for a 1200-segment trail covering only
        // 60 572 pixels, 201 ns per pixel of pure bookkeeping. A value per
        // pixel has almost no fixed cost but scans the full extent of every
        // row, which for a 2400-pixel-wide annotation is 32 bytes of
        // accumulator traffic per pixel before anything is blended.
        //
        // 256 is comfortably above a trail segment or a glyph and far below a
        // large annotation, and the benchmark is flat either side of it.
        constexpr std::size_t   denseRowWidthLimit = 256U;

        [[nodiscard]]
        std::uint64_t
        rectangle_area( geometry::Rectangle rectangle ) noexcept
        {
            return static_cast<std::uint64_t>( rectangle.width ) * rectangle.height;
        }

        [[nodiscard]]
        geometry::Rectangle
        rectangle_union( geometry::Rectangle left,
                         geometry::Rectangle right ) noexcept
        {
            const auto x = std::min( left.x, right.x );
            const auto y = std::min( left.y, right.y );
            const auto right_edge =
                std::max( rectangle_right( left ), rectangle_right( right ) );
            const auto bottom_edge =
                std::max( rectangle_bottom( left ), rectangle_bottom( right ) );
            return geometry::Rectangle{
                .x      = x,
                .y      = y,
                .width  = static_cast<std::uint32_t>( right_edge - x ),
                .height = static_cast<std::uint32_t>( bottom_edge - y ),
            };
        }

        [[nodiscard]]
        bool
        worth_merging( geometry::Rectangle left,
                       geometry::Rectangle right ) noexcept
        {
            return rectangle_area( rectangle_union( left, right ) ) <=
                   rectangle_area( left ) +
                   rectangle_area( right ) +
                   mergeWasteBudget;
        }

        // The frame's damage, clipped to the surface and merged.
        //
        // This used to make the set disjoint by SUBTRACTION: every new
        // rectangle was cut against every rectangle already accepted, and each
        // cut yields up to four pieces. 600 overlapping trail bounds came out
        // as 3236 fragments — more rectangles than there were shapes — and
        // every shape then paid a linear scan of all of them, twice: once in
        // select_damage and once inside the rasterizer.
        //
        // Merging instead of cutting can only shrink the count. Disjointness is
        // no longer required of this function: overlapping rectangles reach the
        // rasterizer, which merges each row's x-runs before it blends, so a
        // pixel still receives exactly one blend per shape.
        [[nodiscard]]
        std::vector<geometry::Rectangle>
        coalesce_damage( std::span<const geometry::Rectangle> damage,
                         geometry::Size                       surface )
        {
            const geometry::Rectangle surface_rectangle{
                .width  = surface.width,
                .height = surface.height,
            };
            std::vector<geometry::Rectangle> merged;
            merged.reserve( damage.size() );
            for( const auto rectangle : damage )
            {
                const auto clipped = intersection( rectangle, surface_rectangle );
                if( !clipped.has_value() || rectangle_area( *clipped ) == 0U )
                {
                    continue;
                }
                auto        candidate = *clipped;
                std::size_t write     = 0;
                for( std::size_t read = 0; read < merged.size(); ++read )
                {
                    if( worth_merging( candidate, merged[read] ) )
                    {
                        candidate = rectangle_union( candidate, merged[read] );
                        continue;
                    }
                    merged[write] = merged[read];
                    ++write;
                }
                merged.resize( write );
                merged.push_back( candidate );
            }
            if( merged.size() > maxDamageRectangles )
            {
                auto bounding = merged.front();
                for( const auto rectangle : merged )
                {
                    bounding = rectangle_union( bounding, rectangle );
                }
                merged.assign( 1U, bounding );
            }
            return merged;
        }

        // Collects the damage rectangles `bounds` overlaps. Replaces an any_of
        // predicate: paint_shapes needs the subset, not just whether one
        // exists, because handing rasterize_contours the whole damage set makes
        // its cost shapes x damage.
        void
        select_damage( geometry::Rectangle                  bounds,
                       std::span<const geometry::Rectangle> damage,
                       std::vector<geometry::Rectangle>&    selected )
        {
            selected.clear();
            for( const auto rectangle : damage )
            {
                if( intersects( bounds, rectangle ) )
                {
                    selected.push_back( rectangle );
                }
            }
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

        // The packed blend works on two channels at a time, in the 16-bit
        // lanes of a 32-bit word: B and R in one word, G and A in the other.
        constexpr std::uint32_t channelMaximumByte = 255U;
        constexpr std::uint32_t evenLaneMask       = 0X00'FF'00'FFU;
        constexpr std::uint32_t roundingLanes      = 0X00'80'00'80U;
        constexpr std::uint32_t laneShift          = 8U;
        constexpr std::uint32_t highLaneShift      = 16U;

        // round(lane / 255) for both 16-bit lanes at once, without a divide.
        //
        // Each lane holds at most 255 x 255 = 65025 and gains at most 382 here,
        // so neither can carry into the other.
        [[nodiscard]]
        std::uint32_t
        divide_lanes_by_channel_maximum( std::uint32_t lanes ) noexcept
        {
            const auto biased = lanes + roundingLanes;
            return ( ( biased + ( ( biased >> laneShift ) & evenLaneMask ) ) >>
                     laneShift ) &
                   evenLaneMask;
        }

        // Blends [first_x, last_x) of row `y` at one constant coverage.
        //
        // This is the innermost loop of the whole overlay and, until this
        // rewrite, the single most expensive thing grab did. It ran per pixel
        // and converted four channels between double and byte each time, having
        // recomputed the source alpha per pixel as well. Measured on a
        // 2400x1600 annotation it was 44.6 ms of a 55.9 ms frame — 80% of the
        // time, for arithmetic that does not vary across a span.
        //
        // Two things fix that. Coverage is constant across a run by
        // construction — the caller only calls this between two coverage
        // events — so the source's premultiplied contribution is computed once
        // for the run. And src-over is exact in integers, two channels at a
        // time, so a pixel costs two multiply-adds and two rounded divides
        // rather than four of each in double precision with eight conversions.
        void
        blend_run( Image&                image,
                   std::size_t           first_x,
                   std::size_t           last_x,
                   std::size_t           y,
                   const overlay::Color& color,
                   double                source_alpha ) noexcept
        {
            if( source_alpha <= fullyTransparent || first_x >= last_x )
            {
                return;
            }
            const auto alpha = static_cast<std::uint32_t>(
                ( std::min( source_alpha, fullyOpaque ) * channelMaximum ) + roundingBias
            );
            if( alpha == 0U )
            {
                return;
            }
            const auto inverse = channelMaximumByte - alpha;

            // The lane layout below is the surface's byte order, not a choice.
            static_assert( blueChannelOffset ==
                               0U &&
                               greenChannelOffset ==
                               1U &&
                               redChannelOffset ==
                               2U &&
                               alphaChannelOffset == 3U,
                           "the packed blend assumes BGRA bytes in memory order" );
            const auto source_blue_red =
                ( static_cast<std::uint32_t>( color.b ) * alpha ) |
                ( ( static_cast<std::uint32_t>( color.r ) * alpha ) << highLaneShift );
            const auto source_green_alpha =
                ( static_cast<std::uint32_t>( color.g ) * alpha ) |
                ( ( channelMaximumByte * alpha ) << highLaneShift );

            const auto row_offset = y * static_cast<std::size_t>( image.stride );
            assert( row_offset + ( last_x * bgraBytesPerPixel ) <= image.pixels.size() );
            auto* const row = std::next( image.pixels.data(),
                                         static_cast<std::ptrdiff_t>( row_offset ) );
            for( auto x = first_x; x < last_x; ++x )
            {
                auto* const pixel =
                    std::next( row,
                               static_cast<std::ptrdiff_t>( x * bgraBytesPerPixel ) );
                // memcpy rather than a cast: the buffer is bytes, and this is
                // the only way to read four of them as a word without leaning
                // on type punning. It compiles to the same single load.
                std::uint32_t destination{};
                std::memcpy( &destination, pixel, sizeof destination );
                const auto blue_red = divide_lanes_by_channel_maximum(
                    ( ( destination & evenLaneMask ) * inverse ) + source_blue_red
                );
                const auto green_alpha = divide_lanes_by_channel_maximum(
                    ( ( ( destination >> laneShift ) & evenLaneMask ) * inverse ) +
                    source_green_alpha
                );
                const auto blended = blue_red | ( green_alpha << laneShift );
                std::memcpy( pixel, &blended, sizeof blended );
            }
        }

        // One directed edge of a flattened contour, in the form a scanline
        // walk wants: keyed by its top, carrying the x it starts at and the
        // slope to advance by.
        //
        // Building this once per shape is what replaced walking every point of
        // every contour once per sub-scanline. At 8 sub-scanlines per row that
        // walk was the shape of the entire cost, and it did it with two
        // bounds-checked `.at()` accesses and a modulo per edge.
        struct Edge
        {
                double y_top{};
                double y_bottom{};
                double x_at_top{};
                double slope{};
                int    winding{};
        };

        // A half-open [first, last) run of pixels within one row.
        struct XInterval
        {
                std::size_t first{};
                std::size_t last{};
        };

        // Working memory for the scanline fill, owned by the raster and reused
        // across frames: a busy trail rasterizes several hundred shapes per
        // frame and none of them should allocate.
        // Where a run of fully covered pixels starts or stops, on one row.
        struct CoverageEvent
        {
                std::size_t x{};
                double      weight{};
        };

        // A pixel one of the sub-scanlines covers only partly — the two ends of
        // a span. There are at most two per sub-scanline span, so a row carries
        // a handful of these however wide the shape is.
        struct PartialPixel
        {
                std::size_t x{};
                double      coverage{};
        };

        struct RasterScratch
        {
                std::vector<Edge>          edges;
                std::vector<std::size_t>   active;
                std::vector<Crossing>      crossings;
                // A row's coverage as events and partial pixels rather than one
                // value per pixel. The interior of a span is two events, not
                // 2400 accumulator slots, so nothing per-pixel is written
                // before the blend.
                std::vector<CoverageEvent> events;
                std::vector<PartialPixel>  partials;
                // The same coverage as a value per pixel, for rows too narrow
                // to pay for the event bookkeeping. Both are one longer than
                // the surface is wide: a span may end exactly at the right edge
                // and its closing delta lands one past it.
                std::vector<double>        coverage;
                std::vector<double>        delta;
                std::vector<XInterval>     row_intervals;
        };

        void
        build_edges( const FlatContours& contours,
                     std::vector<Edge>&  edges )
        {
            edges.clear();
            for( const auto& contour : contours.contours )
            {
                if( contour.count < 2U )
                {
                    continue;
                }
                for( std::size_t offset{}; offset < contour.count; ++offset )
                {
                    const auto next_offset =
                        offset + 1U == contour.count ? std::size_t{} : offset + 1U;
                    const auto start = contours.points[contour.first + offset];
                    const auto end   = contours.points[contour.first + next_offset];
                    // A horizontal edge crosses no sub-scanline. The half-open
                    // test this replaces skipped it too, by never matching.
                    if( start.y == end.y )
                    {
                        continue;
                    }
                    const bool  downward = start.y < end.y;
                    const auto& upper    = downward ? start : end;
                    const auto& lower    = downward ? end : start;
                    edges.push_back( Edge{
                        .y_top    = upper.y,
                        .y_bottom = lower.y,
                        .x_at_top = upper.x,
                        .slope    = ( lower.x - upper.x ) / ( lower.y - upper.y ),
                        .winding  = downward ? 1 : -1,
                    } );
                }
            }
            std::ranges::sort( edges, {}, &Edge::y_top );
        }

        // Records one covered interval of a sub-scanline against the row.
        //
        // Only the two partial end pixels are recorded individually. The fully
        // covered middle becomes a pair of events, so a 2400-pixel-wide
        // interior costs two entries per sub-scanline instead of 2400
        // accumulations — and, once the row is resolved, one blended run
        // instead of 2400 blended pixels.
        void
        add_span( RasterScratch& scratch,
                  bool           dense,
                  double         span_start,
                  double         span_end,
                  std::size_t    first_pixel,
                  std::size_t    last_pixel,
                  double         weight )
        {
            const auto clipped_start =
                std::max( span_start, static_cast<double>( first_pixel ) );
            const auto clipped_end =
                std::min( span_end, static_cast<double>( last_pixel ) );
            if( clipped_end <= clipped_start )
            {
                return;
            }
            // Both ends are non-negative and clamped into the row, so a
            // truncating cast is floor.
            const auto first = static_cast<std::size_t>( clipped_start );
            const auto last  = static_cast<std::size_t>( clipped_end );
            const auto head  = static_cast<double>( first + 1U ) - clipped_start;
            const auto tail  = clipped_end - static_cast<double>( last );
            if( first == last )
            {
                const auto only = ( clipped_end - clipped_start ) * weight;
                if( dense )
                {
                    scratch.coverage[first] += only;
                }
                else
                {
                    scratch.partials.push_back( PartialPixel{
                        .x        = first,
                        .coverage = only,
                    } );
                }
                return;
            }
            if( dense )
            {
                scratch.coverage[first]   += head * weight;
                scratch.delta[first + 1U] += weight;
                scratch.delta[last]       -= weight;
                if( tail > fullyTransparent )
                {
                    scratch.coverage[last] += tail * weight;
                }
                return;
            }
            scratch.partials.push_back( PartialPixel{
                .x        = first,
                .coverage = head * weight,
            } );
            if( last > first + 1U )
            {
                scratch.events.push_back( CoverageEvent{
                    .x      = first + 1U,
                    .weight = weight,
                } );
                scratch.events.push_back( CoverageEvent{
                    .x      = last,
                    .weight = -weight,
                } );
            }
            if( tail > fullyTransparent && last < last_pixel )
            {
                scratch.partials.push_back( PartialPixel{
                    .x        = last,
                    .coverage = tail * weight,
                } );
            }
        }

        // The x-runs of `row` that lie inside the damage set, merged so that no
        // pixel appears in two of them.
        //
        // Damage rectangles are allowed to overlap — see coalesce_damage — and
        // blending one pixel twice for the same shape darkens it. Merging here,
        // in one dimension, is exact and costs a sort of two or three entries;
        // making the rectangles disjoint in two dimensions instead is what used
        // to turn 600 trail bounds into 3236 fragments.
        void
        collect_row_intervals( std::span<const geometry::Rectangle> damage,
                               std::size_t                          row,
                               std::size_t                          first_pixel,
                               std::size_t                          last_pixel,
                               std::vector<XInterval>&              intervals )
        {
            intervals.clear();
            const auto row_index = static_cast<std::int64_t>( row );
            for( const auto rectangle : damage )
            {
                if( row_index <
                    rectangle.y ||
                    row_index >= rectangle_bottom( rectangle ) )
                {
                    continue;
                }
                const auto left =
                    std::max( first_pixel, static_cast<std::size_t>( rectangle.x ) );
                const auto right =
                    std::min( last_pixel,
                              static_cast<std::size_t>( rectangle_right( rectangle ) ) );
                if( right > left )
                {
                    intervals.push_back( XInterval{ .first = left, .last = right } );
                }
            }
            if( intervals.size() < 2U )
            {
                return;
            }
            std::ranges::sort( intervals, {}, &XInterval::first );
            std::size_t write = 0;
            for( std::size_t read = 1U; read < intervals.size(); ++read )
            {
                if( intervals[read].first <= intervals[write].last )
                {
                    intervals[write].last =
                        std::max( intervals[write].last, intervals[read].last );
                }
                else
                {
                    ++write;
                    intervals[write] = intervals[read];
                }
            }
            intervals.resize( write + 1U );
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

        // A row's coverage, read out of the dense accumulators as runs.
        //
        // Flush whenever a pixel's coverage differs from the run being built,
        // so the blend still happens a run at a time even though the coverage
        // was accumulated one pixel at a time. Both accumulators are reset on
        // the way past, so the next row starts clean without a second sweep.
        template<typename Emit>
        void
        resolve_dense_row( RasterScratch& scratch,
                           std::size_t    first_pixel,
                           std::size_t    last_pixel,
                           Emit&&         emit )
        {
            double      running   = fullyTransparent;
            std::size_t run_first = first_pixel;
            double      run_value = fullyTransparent;
            for( auto x = first_pixel; x < last_pixel; ++x )
            {
                const auto step      = scratch.delta[x];
                const auto own       = scratch.coverage[x];
                scratch.delta[x]     = fullyTransparent;
                scratch.coverage[x]  = fullyTransparent;
                running             += step;
                const auto value     = running + own;
                if( value != run_value )
                {
                    emit( run_first, x, run_value );
                    run_first = x;
                    run_value = value;
                }
            }
            emit( run_first, last_pixel, run_value );
            // A span ending exactly at the right edge closes one past it.
            scratch.delta[last_pixel] = fullyTransparent;
        }

        // The same runs, read out of coverage events and partial pixels.
        //
        // Between two events every pixel has the same coverage, so the walk
        // jumps from boundary to boundary and the interior of a shape never
        // costs anything per pixel until it is blended. Only the pixels a
        // sub-scanline cut through are handled one at a time, and there are at
        // most two of those per span.
        template<typename Emit>
        void
        resolve_sparse_row( RasterScratch& scratch,
                            std::size_t    first_pixel,
                            std::size_t    last_pixel,
                            Emit&&         emit )
        {
            std::ranges::sort( scratch.events, {}, &CoverageEvent::x );
            std::ranges::sort( scratch.partials, {}, &PartialPixel::x );

            double      accumulated   = fullyTransparent;
            std::size_t cursor        = first_pixel;
            std::size_t event_index   = 0;
            std::size_t partial_index = 0;
            while( cursor < last_pixel )
            {
                auto boundary = last_pixel;
                if( event_index < scratch.events.size() )
                {
                    boundary = std::min( boundary, scratch.events[event_index].x );
                }
                if( partial_index < scratch.partials.size() )
                {
                    boundary = std::min( boundary, scratch.partials[partial_index].x );
                }
                if( boundary > cursor )
                {
                    emit( cursor, boundary, accumulated );
                    cursor = boundary;
                    continue;
                }
                while( event_index <
                       scratch.events.size() &&
                       scratch.events[event_index].x == cursor )
                {
                    accumulated += scratch.events[event_index].weight;
                    ++event_index;
                }
                if( partial_index >=
                    scratch.partials.size() ||
                    scratch.partials[partial_index].x != cursor )
                {
                    continue;
                }
                auto extra = fullyTransparent;
                while( partial_index <
                       scratch.partials.size() &&
                       scratch.partials[partial_index].x == cursor )
                {
                    extra += scratch.partials[partial_index].coverage;
                    ++partial_index;
                }
                emit( cursor, cursor + 1U, accumulated + extra );
                ++cursor;
            }
        }

        void
        rasterize_contours( const FlatContours&                  contours,
                            const overlay::Color&                color,
                            double                               opacity,
                            const std::optional<RevealMask>&     reveal,
                            std::span<const geometry::Rectangle> damage,
                            Image&                               image,
                            RasterScratch&                       scratch,
                            RenderProfile*                       profile )
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

            // Narrow to the damage this shape actually reaches, once, before
            // touching an edge. This used to be a loop that re-ran the entire
            // scanline machinery per damage rectangle the shape overlapped:
            // with the damage set fragmented into thousands of pieces, a
            // 600-segment trail made 5840 rasterize calls to paint 20 636
            // pixels — 3.5 pixels of work behind 2.5 us of setup each.
            auto covered_first_x = last_x;
            auto covered_last_x  = first_x;
            auto covered_first_y = last_y;
            auto covered_last_y  = first_y;
            for( const auto clip : damage )
            {
                assert( clip.x >= 0 );
                assert( clip.y >= 0 );
                const auto clip_first_x =
                    std::max( first_x, static_cast<std::size_t>( clip.x ) );
                const auto clip_last_x =
                    std::min( last_x,
                              static_cast<std::size_t>( rectangle_right( clip ) ) );
                const auto clip_first_y =
                    std::max( first_y, static_cast<std::size_t>( clip.y ) );
                const auto clip_last_y =
                    std::min( last_y,
                              static_cast<std::size_t>( rectangle_bottom( clip ) ) );
                if( clip_first_x >= clip_last_x || clip_first_y >= clip_last_y )
                {
                    continue;
                }
                covered_first_x = std::min( covered_first_x, clip_first_x );
                covered_last_x  = std::max( covered_last_x, clip_last_x );
                covered_first_y = std::min( covered_first_y, clip_first_y );
                covered_last_y  = std::max( covered_last_y, clip_last_y );
            }
            if( covered_first_x >= covered_last_x || covered_first_y >= covered_last_y )
            {
                return;
            }

            build_edges( contours, scratch.edges );
            if( scratch.edges.empty() )
            {
                return;
            }
            if( profile != nullptr )
            {
                profile->rasterize_calls += 1U;
            }

            constexpr auto sampleCentre =
                fullyOpaque / static_cast<double>( circleHalves );
            const auto sample_weight =
                fullyOpaque / static_cast<double>( antiAliasSubscanlines );
            // Everything about the source that does not vary across the shape,
            // computed once instead of per pixel.
            const auto base_alpha =
                ( static_cast<double>( color.a ) / channelMaximum ) * opacity;
            // A reveal along X is the one thing that makes coverage differ
            // between neighbouring pixels of an otherwise constant run.
            const bool reveal_varies = reveal.has_value() &&
                                       reveal_is_partial( reveal ) &&
                                       reveal->axis == overlay::Axis::X;
            const bool dense_rows =
                covered_last_x - covered_first_x <= denseRowWidthLimit;

            scratch.active.clear();
            std::size_t next_edge = 0;

            for( auto y = covered_first_y; y < covered_last_y; ++y )
            {
                const auto row_top    = static_cast<double>( y );
                const auto row_bottom = row_top + fullyOpaque;

                // The active edge table. Edges arrive in y_top order and leave
                // when the row passes their bottom, so each is examined once on
                // the way in and once on the way out rather than once per
                // sub-scanline of every row it does not touch.
                while( next_edge <
                       scratch.edges.size() &&
                       scratch.edges[next_edge].y_top < row_bottom )
                {
                    scratch.active.push_back( next_edge );
                    ++next_edge;
                }
                std::erase_if( scratch.active,
                               [&scratch, row_top]( std::size_t index )
                               {
                                   return scratch.edges[index].y_bottom <= row_top;
                               } );

                collect_row_intervals( damage,
                                       y,
                                       covered_first_x,
                                       covered_last_x,
                                       scratch.row_intervals );
                if( scratch.row_intervals.empty() || scratch.active.empty() )
                {
                    continue;
                }
                if( profile != nullptr )
                {
                    for( const auto interval : scratch.row_intervals )
                    {
                        profile->covered_pixels += interval.last - interval.first;
                    }
                }

                scratch.events.clear();
                scratch.partials.clear();
                for( std::size_t sample{}; sample < antiAliasSubscanlines; ++sample )
                {
                    const auto sample_y =
                        row_top + ( ( static_cast<double>( sample ) + sampleCentre ) /
                                    static_cast<double>( antiAliasSubscanlines ) );
                    if( !sample_passes_reveal( sample_y, reveal ) )
                    {
                        continue;
                    }
                    scratch.crossings.clear();
                    for( const auto index : scratch.active )
                    {
                        const auto& edge = scratch.edges[index];
                        if( edge.y_top > sample_y || sample_y >= edge.y_bottom )
                        {
                            continue;
                        }
                        scratch.crossings.push_back( Crossing{
                            .x             = edge.x_at_top +
                                             ( ( sample_y - edge.y_top ) * edge.slope ),
                            .winding_delta = edge.winding,
                        } );
                    }
                    if( scratch.crossings.size() < circleHalves )
                    {
                        continue;
                    }
                    std::ranges::sort( scratch.crossings, {}, &Crossing::x );
                    int    winding{};
                    double previous_x{};
                    bool   has_previous{};
                    for( const auto crossing : scratch.crossings )
                    {
                        if( has_previous && winding != 0 )
                        {
                            add_span( scratch,
                                      dense_rows,
                                      previous_x,
                                      crossing.x,
                                      covered_first_x,
                                      covered_last_x,
                                      sample_weight );
                        }
                        winding      += crossing.winding_delta;
                        previous_x    = crossing.x;
                        has_previous  = true;
                    }
                }

                // Resolve the row into constant-coverage runs and blend them.
                //
                // The two representations have opposite failure modes, so the
                // shape picks one and both end at the same `emit`.
                const auto emit =
                    [&scratch, &image, &color, reveal_varies, &reveal, y, base_alpha](
                        std::size_t run_first,
                        std::size_t run_last,
                        double      coverage
                    )
                {
                    const auto clamped =
                        std::clamp( coverage, fullyTransparent, fullyOpaque );
                    if( clamped <= fullyTransparent || run_first >= run_last )
                    {
                        return;
                    }
                    for( const auto interval : scratch.row_intervals )
                    {
                        const auto blend_first = std::max( run_first, interval.first );
                        const auto blend_last  = std::min( run_last, interval.last );
                        if( blend_first >= blend_last )
                        {
                            continue;
                        }
                        if( reveal_varies )
                        {
                            // A partial reveal along X scales every pixel
                            // differently, so the run cannot share one alpha.
                            for( auto x = blend_first; x < blend_last; ++x )
                            {
                                blend_run( image,
                                           x,
                                           x + 1U,
                                           y,
                                           color,
                                           base_alpha *
                                               clamped *
                                               horizontal_reveal_coverage( x, reveal ) );
                            }
                            continue;
                        }
                        blend_run( image,
                                   blend_first,
                                   blend_last,
                                   y,
                                   color,
                                   base_alpha * clamped );
                    }
                };

                if( dense_rows )
                {
                    resolve_dense_row( scratch, covered_first_x, covered_last_x, emit );
                }
                else
                {
                    resolve_sparse_row( scratch, covered_first_x, covered_last_x, emit );
                }
            }
        }

        void
        paint_shapes( std::span<const TrackedShape>        shapes,
                      std::span<const FlatContours>        geometries,
                      std::span<const geometry::Rectangle> damage,
                      Image&                               image,
                      RasterScratch&                       scratch,
                      std::vector<geometry::Rectangle>&    shape_damage,
                      RenderProfile*                       profile )
        {
            assert( shapes.size() == geometries.size() );
            auto geometry_iterator = geometries.begin();
            for( const auto& tracked : shapes )
            {
                const auto& geometry = *geometry_iterator;
                ++geometry_iterator;
                if( !tracked.bounds.has_value() )
                {
                    continue;
                }

                // Hand rasterize_contours only the damage rectangles this
                // shape actually touches, so a frame that repainted one corner
                // does not walk the whole damage set per shape.
                select_damage( *tracked.bounds, damage, shape_damage );
                if( shape_damage.empty() )
                {
                    continue;
                }

                if( tracked.record.shape.fill.has_value() )
                {
                    const auto started = profile_now( profile );
                    rasterize_contours( geometry,
                                        tracked.record.shape.fill->color,
                                        tracked.opacity,
                                        tracked.reveal,
                                        shape_damage,
                                        image,
                                        scratch,
                                        profile );
                    profile_add( profile, &RenderProfile::fill, started );
                }
                if( tracked.record.shape.stroke.has_value() &&
                    std::isfinite( tracked.record.shape.stroke->width_px ) &&
                    tracked.record.shape.stroke->width_px > fullyTransparent )
                {
                    const auto outline_started = profile_now( profile );
                    const auto outline         = stroke_outline(
                        geometry,
                        static_cast<double>( tracked.record.shape.stroke->width_px )
                    );
                    profile_add( profile, &RenderProfile::outline, outline_started );
                    const auto started = profile_now( profile );
                    rasterize_contours( outline,
                                        tracked.record.shape.stroke->color,
                                        tracked.opacity,
                                        tracked.reveal,
                                        shape_damage,
                                        image,
                                        scratch,
                                        profile );
                    profile_add( profile, &RenderProfile::fill, started );
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
                }
            {
                scratch.coverage.assign( raster_size.width + 1U, 0.0 );
                scratch.delta.assign( raster_size.width + 1U, 0.0 );
            }

            Image                            image;
            RasterScratch                    scratch;
            std::vector<TrackedShape>        previous_shapes;
            // Per-shape subset of the frame's damage. Lives here so it keeps
            // its capacity across frames instead of allocating per shape.
            std::vector<geometry::Rectangle> shape_damage;
            bool                             full_redraw_required{ true };
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

            // Measure only when someone is listening. The phase timers are two
            // clock reads each and the per-shape ones run inside the loop that
            // is being measured, so an always-on profile would be part of what
            // it reports.
            RenderProfile        measured;
            RenderProfile* const profile = log::enabled( log::Level::Verbose ) &&
                                                   log::runtime_level() >=
                                                   log::Level::Verbose
                                             ? &measured
                                             : nullptr;
            const auto           evaluate_started = profile_now( profile );

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

            profile_add( profile, &RenderProfile::evaluate, evaluate_started );

            auto normalized_damage      = coalesce_damage( damage, impl_->image.size() );
            impl_->full_redraw_required = true;
            const auto clear_started    = profile_now( profile );
            clear_damage( impl_->image, normalized_damage );
            profile_add( profile, &RenderProfile::clear, clear_started );

            // paint_shapes is O(shapes x damage rects) before it touches a
            // pixel, and rasterizes a shape once per damage rect it overlaps.
            // These two counts are what tell you whether a slow frame is
            // shape-bound or pixel-bound.
            const diag::Scope<log::Level::Verbose> paint_scope;
            paint_shapes( current_shapes,
                          flattened_shapes,
                          normalized_damage,
                          impl_->image,
                          impl_->scratch,
                          impl_->shape_damage,
                          profile );

            // One line per frame carrying the whole cost model: how many shapes
            // and how many pixels, then where the time went between diffing the
            // scene, clearing damage, building stroke outlines and filling.
            // Reading it is how "the overlay is laggy" becomes a decision about
            // which of those four to change.
            log::verbose(
                [&current_shapes,
                 &flattened_shapes,
                 &normalized_damage,
                 &paint_scope,
                 &measured]( auto& event )
                {
                    std::size_t points = 0;
                    for( const auto& flattened : flattened_shapes )
                    {
                        points += flattened.points.size();
                    }
                    const auto milliseconds = []( std::chrono::nanoseconds span )
                    {
                        return std::chrono::duration<double, std::milli>( span ).count();
                    };
                    event.tag( log::tags::raster )
                        .value( "shapes", current_shapes.size() )
                        .value( "damage_rects", normalized_damage.size() )
                        .value( "contour_points", points )
                        .value( "covered_px", measured.covered_pixels )
                        .value( "raster_calls", measured.rasterize_calls )
                        .value( "evaluate_ms", milliseconds( measured.evaluate ) )
                        .value( "clear_ms", milliseconds( measured.clear ) )
                        .value( "outline_ms", milliseconds( measured.outline ) )
                        .value( "fill_ms", milliseconds( measured.fill ) )
                        .value( "paint_ms", milliseconds( paint_scope.elapsed() ) );
                }
            );

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
