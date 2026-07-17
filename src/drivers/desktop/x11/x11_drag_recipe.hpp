#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/drag.hpp"
#include "grab/geometry/curve.hpp"
#include "grab/geometry/point.hpp"
#include "grab/result.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace grab::drivers::desktop::x11
{

    namespace detail
    {

        [[nodiscard]]
        inline bool
        drag_point_in_int16_range( grab::geometry::Point point ) noexcept
        {
            return point.x >=
                   std::numeric_limits<std::int16_t>::min() &&
                   point.x <=
                   std::numeric_limits<std::int16_t>::max() &&
                   point.y >=
                   std::numeric_limits<std::int16_t>::min() &&
                   point.y <= std::numeric_limits<std::int16_t>::max();
        }

        [[nodiscard]]
        inline grab::geometry::Point
        drag_interpolated_point( grab::geometry::Point from,
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
        drag_waypoints( grab::geometry::Point           from,
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
                                                  start.translated( control_offset,
                                                                    0.0 ),
                                                  end.translated( -control_offset, 0.0 ),
                                                  end );
                const auto sampled =
                    curve.sample( static_cast<std::size_t>( steps ) + 1U );
                for( std::size_t index = 1U; index < sampled.size(); ++index )
                {
                    points.push_back( sampled[index] );
                }
            }
            else
            {
                for( std::int32_t step = 1; step <= steps; ++step )
                {
                    points.push_back( drag_interpolated_point( from, to, step, steps ) );
                }
            }
            return points;
        }

    }    // namespace detail

    // Press-move-release drag recipe. `SeatT` is any seat exposing
    // move_pointer_absolute(std::int16_t,std::int16_t), button(std::uint8_t,bool),
    // and flush(), each returning grab::Result<void> (both grab::input::Seat and
    // X11InputSeat qualify). SLEEPLESS by design: the project forbids raw sleeps
    // under src/, so DragOptions.step_dwell no longer paces the motion; the drag
    // still walks options.interpolation_steps interpolated waypoints (linear or
    // cubic per options.path).
    template<typename SeatT>
    [[nodiscard]]
    grab::Result<void>
    execute_drag( SeatT&                          seat,
                  grab::geometry::Point           from,
                  grab::geometry::Point           to,
                  const grab::input::DragOptions& options )
    {
        constexpr std::uint8_t primary_button = 1U;

        if( options.interpolation_steps <
            grab::input::DragOptions::minimumInterpolationSteps ||
            options.interpolation_steps >
            grab::input::DragOptions::maximumInterpolationSteps )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "drag interpolation-step count is out of range" );
        }
        if( options.step_dwell < std::chrono::milliseconds::zero() )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "drag dwell must not be negative" );
        }
        if( !detail::drag_point_in_int16_range( from ) ||
            !detail::drag_point_in_int16_range( to ) )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "drag coordinate is outside int16 range" );
        }

        const auto waypoints = detail::drag_waypoints( from, to, options );
        for( const auto point : waypoints )
        {
            if( !detail::drag_point_in_int16_range( point ) )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "drag coordinate is outside int16 range" );
            }
        }

        auto result = seat.move_pointer_absolute( static_cast<std::int16_t>( from.x ),
                                                  static_cast<std::int16_t>( from.y ) );
        if( !result )
        {
            return result;
        }
        result = seat.flush();
        if( !result )
        {
            return result;
        }

        result = seat.button( primary_button, true );
        if( !result )
        {
            return grab::fail( grab::ErrorCode::PossiblyCommitted,
                               "drag failed after crossing the input commit boundary" );
        }

        for( const auto point : waypoints )
        {
            result = seat.move_pointer_absolute( static_cast<std::int16_t>( point.x ),
                                                 static_cast<std::int16_t>( point.y ) );
            if( !result )
            {
                return grab::fail(
                    grab::ErrorCode::PossiblyCommitted,
                    "drag failed after crossing the input commit boundary"
                );
            }
            result = seat.flush();
            if( !result )
            {
                return grab::fail(
                    grab::ErrorCode::PossiblyCommitted,
                    "drag failed after crossing the input commit boundary"
                );
            }
        }

        result = seat.button( primary_button, false );
        if( !result )
        {
            return grab::fail( grab::ErrorCode::PossiblyCommitted,
                               "drag failed after crossing the input commit boundary" );
        }
        result = seat.flush();
        if( !result )
        {
            return grab::fail( grab::ErrorCode::PossiblyCommitted,
                               "drag failed after crossing the input commit boundary" );
        }
        return {};
    }

}    // namespace grab::drivers::desktop::x11
