#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

// The interpolation walk, lifted out of the X11 drag recipe so that a move, a
// curve follow and a drag are one implementation rather than three. It was
// welded to a button press; nothing about producing waypoints is.
//
// The returned points EXCLUDE `from` and INCLUDE `to`, and there are exactly
// options.interpolation_steps of them. That is the recipe's existing contract,
// preserved verbatim: the last linear point is from + (delta * steps) / steps,
// which lands exactly on `to` with no rounding drift.
//
// This produces geometry only. It does no pacing and touches no clock — the
// project forbids raw sleeps under src/, and anything that needs to wait does
// so through a deadline, never by sleeping.

#include "grab/drag.hpp"
#include "grab/geometry/curve.hpp"
#include "grab/geometry/point.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace grab::kernel::input
{

    [[nodiscard]]
    inline grab::geometry::Point
    interpolated_point( grab::geometry::Point from,
                        grab::geometry::Point to,
                        std::int32_t          step,
                        std::int32_t          step_count ) noexcept
    {
        const auto start_x = static_cast<std::int64_t>( from.x );
        const auto start_y = static_cast<std::int64_t>( from.y );
        const auto dx      = static_cast<std::int64_t>( to.x ) - start_x;
        const auto dy      = static_cast<std::int64_t>( to.y ) - start_y;
        const auto count   = static_cast<std::int64_t>( step_count );
        const auto current = static_cast<std::int64_t>( step );
        return grab::geometry::Point{
            .x = static_cast<std::int32_t>( start_x + ( ( dx * current ) / count ) ),
            .y = static_cast<std::int32_t>( start_y + ( ( dy * current ) / count ) ),
        };
    }

    [[nodiscard]]
    inline std::vector<grab::geometry::Point>
    waypoints( grab::geometry::Point           from,
               grab::geometry::Point           to,
               const grab::input::DragOptions& options )
    {
        std::vector<grab::geometry::Point> points;
        const auto                         steps = options.interpolation_steps;
        if( options.path == grab::input::DragOptions::Path::Cubic )
        {
            const grab::geometry::PointF start{
                .x = static_cast<double>( from.x ),
                .y = static_cast<double>( from.y ),
            };
            const grab::geometry::PointF end{
                .x = static_cast<double>( to.x ),
                .y = static_cast<double>( to.y ),
            };
            const double control_offset = ( end.x - start.x ) / 3.0;
            const auto   curve =
                grab::geometry::Curve::cubic( start,
                                              start.translated( control_offset, 0.0 ),
                                              end.translated( -control_offset, 0.0 ),
                                              end );
            const auto sampled = curve.sample( static_cast<std::size_t>( steps ) + 1U );
            for( std::size_t index = 1U; index < sampled.size(); ++index )
            {
                points.push_back( sampled[index] );
            }
        }
        else
        {
            for( std::int32_t step = 1; step <= steps; ++step )
            {
                points.push_back( interpolated_point( from, to, step, steps ) );
            }
        }
        return points;
    }

}    // namespace grab::kernel::input
