#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/drag.hpp"
#include "grab/geometry/point.hpp"
#include "grab/result.hpp"
#include "kernel/input/waypoints.hpp"

#include <chrono>
#include <cstdint>
#include <limits>

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

        const auto waypoints = grab::kernel::input::waypoints( from, to, options );
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
