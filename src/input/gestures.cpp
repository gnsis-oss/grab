#include "grab/geometry/curve.hpp"
#include "grab/geometry/point.hpp"
#include "grab/pointer_button.hpp"
#include "grab/result.hpp"
#include "input/gestures.hpp"
#include "input/seat.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <thread>
#include <utility>
#include <vector>

namespace grab::input
{

    namespace
    {

        constexpr double cubicControlDivisor = 3.0;

        [[nodiscard]]
        grab::Result<void>
        move_error( grab::Result<void>& result )
        {
            return std::unexpected( std::move( result.error() ) );
        }

        [[nodiscard]]
        grab::Result<void>
        validate_point( Point point )
        {
            if( point.x <
                std::numeric_limits<std::int16_t>::min() ||
                point.x >
                std::numeric_limits<std::int16_t>::max() ||
                point.y <
                std::numeric_limits<std::int16_t>::min() ||
                point.y > std::numeric_limits<std::int16_t>::max() )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "drag coordinate is outside int16 range" );
            }
            return {};
        }

        [[nodiscard]]
        Point
        interpolated( Point        from,
                      Point        to,
                      std::int32_t step,
                      std::int32_t step_count ) noexcept
        {
            const auto start_x = static_cast<std::int64_t>( from.x );
            const auto start_y = static_cast<std::int64_t>( from.y );
            const auto dx      = static_cast<std::int64_t>( to.x ) - start_x;
            const auto dy      = static_cast<std::int64_t>( to.y ) - start_y;
            const auto count   = static_cast<std::int64_t>( step_count );
            const auto current = static_cast<std::int64_t>( step );

            return Point{
                .x = static_cast<std::int16_t>( start_x + ( ( dx * current ) / count ) ),
                .y = static_cast<std::int16_t>( start_y + ( ( dy * current ) / count ) ),
            };
        }

        [[nodiscard]]
        grab::Result<void>
        move_and_flush( Seat& seat,
                        Point point )
        {
            auto move_result =
                seat.move_pointer_absolute( static_cast<std::int16_t>( point.x ),
                                            static_cast<std::int16_t>( point.y ) );
            if( !move_result.has_value() )
            {
                return move_error( move_result );
            }

            auto flush_result = seat.flush();
            if( !flush_result.has_value() )
            {
                return move_error( flush_result );
            }

            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        button( Seat&        seat,
                std::uint8_t button_detail,
                bool         press )
        {
            auto button_result = seat.button( button_detail, press );
            if( !button_result.has_value() )
            {
                return move_error( button_result );
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        release_and_flush( Seat& seat )
        {
            auto button_result = button( seat, primaryButton, false );
            if( !button_result.has_value() )
            {
                return button_result;
            }

            auto flush_result = seat.flush();
            if( !flush_result.has_value() )
            {
                return move_error( flush_result );
            }

            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        release_after_error( Seat&       seat,
                             grab::Error error )
        {
            auto release_result = release_and_flush( seat );
            static_cast<void>( release_result );
            return std::unexpected( std::move( error ) );
        }

        [[nodiscard]]
        grab::Result<void>
        validate_drag_points( Point from,
                              Point to )
        {
            auto from_result = validate_point( from );
            if( !from_result.has_value() )
            {
                return from_result;
            }
            auto to_result = validate_point( to );
            if( !to_result.has_value() )
            {
                return to_result;
            }
            return {};
        }

        [[nodiscard]]
        grab::geometry::PointF
        floating_point( Point point ) noexcept
        {
            return grab::geometry::PointF{
                .x = static_cast<double>( point.x ),
                .y = static_cast<double>( point.y ),
            };
        }

        [[nodiscard]]
        grab::geometry::Curve
        curve_path( Point from,
                    Point to )
        {
            const auto start = floating_point( from );
            const auto end   = floating_point( to );
            const auto dx    = ( end.x - start.x ) / cubicControlDivisor;
            return grab::geometry::Curve::cubic( start,
                                                 start.translated( dx, 0.0 ),
                                                 end.translated( -dx, 0.0 ),
                                                 end );
        }

        [[nodiscard]]
        grab::Result<void>
        validate_points( const std::vector<Point>& points )
        {
            for( const Point point : points )
            {
                auto validated = validate_point( point );
                if( !validated.has_value() )
                {
                    return std::unexpected( std::move( validated.error() ) );
                }
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        move_drag_step( Seat&                            seat,
                        Point                            point,
                        const std::chrono::milliseconds& step_dwell )
        {
            auto move_result = move_and_flush( seat, point );
            if( !move_result.has_value() )
            {
                return move_result;
            }

            std::this_thread::sleep_for( step_dwell );
            return {};
        }

    }    // namespace

    grab::Result<void>
    linear_drag( Seat&              seat,
                 Point              from,
                 Point              to,
                 const DragOptions& options )
    {
        if( options.interpolation_steps <
            DragOptions::minimumInterpolationSteps ||
            options.interpolation_steps > DragOptions::maximumInterpolationSteps )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "linear drag interpolation-step count is out of range" );
        }
        if( options.step_dwell < std::chrono::milliseconds::zero() )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "linear drag dwell must not be negative" );
        }

        auto validation_result = validate_drag_points( from, to );
        if( !validation_result.has_value() )
        {
            return validation_result;
        }

        auto move_result = move_and_flush( seat, from );
        if( !move_result.has_value() )
        {
            return move_result;
        }

        auto press_result = button( seat, primaryButton, true );
        if( !press_result.has_value() )
        {
            return press_result;
        }

        for( std::int32_t step = 1; step <= options.interpolation_steps; ++step )
        {
            const Point point =
                interpolated( from, to, step, options.interpolation_steps );
            auto step_result = move_drag_step( seat, point, options.step_dwell );
            if( !step_result.has_value() )
            {
                return release_after_error( seat, std::move( step_result.error() ) );
            }
        }

        return release_and_flush( seat );
    }

    grab::Result<void>
    curve_drag( Seat&              seat,
                Point              from,
                Point              to,
                const DragOptions& options )
    {
        if( options.interpolation_steps <
            DragOptions::minimumInterpolationSteps ||
            options.interpolation_steps > DragOptions::maximumInterpolationSteps )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "curve drag interpolation-step count is out of range" );
        }
        if( options.step_dwell < std::chrono::milliseconds::zero() )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "curve drag dwell must not be negative" );
        }
        auto validation_result = validate_drag_points( from, to );
        if( !validation_result.has_value() )
        {
            return validation_result;
        }

        const auto sample_count =
            static_cast<std::size_t>( options.interpolation_steps ) + 1U;
        const std::vector<Point> path = curve_path( from, to ).sample( sample_count );
        auto                     path_validation = validate_points( path );
        if( !path_validation.has_value() )
        {
            return path_validation;
        }

        auto move_result = move_and_flush( seat, from );
        if( !move_result.has_value() )
        {
            return move_result;
        }

        auto press_result = button( seat, primaryButton, true );
        if( !press_result.has_value() )
        {
            return press_result;
        }

        for( std::size_t index = 1U; index < path.size(); ++index )
        {
            auto step_result =
                move_drag_step( seat, path.at( index ), options.step_dwell );
            if( !step_result.has_value() )
            {
                return release_after_error( seat, std::move( step_result.error() ) );
            }
        }

        return release_and_flush( seat );
    }

    grab::Result<void>
    menu_click( Seat& seat,
                Point item )
    {
        auto move_result = move_and_flush( seat, item );
        if( !move_result.has_value() )
        {
            return move_result;
        }

        auto press_result = button( seat, primaryButton, true );
        if( !press_result.has_value() )
        {
            return press_result;
        }

        return release_and_flush( seat );
    }

}    // namespace grab::input
