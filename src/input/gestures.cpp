#include "grab/result.hpp"
#include "input/gestures.hpp"
#include "input/seat.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <limits>
#include <thread>
#include <utility>

namespace grab::input
{

    namespace
    {

        constexpr std::uint8_t leftButton                = 1U;
        constexpr std::int16_t armVerticalOffset         = 6;
        constexpr std::int32_t qtStartDragDistance       = 20;
        constexpr std::int32_t minimumInterpolationSteps = 1;
        constexpr auto         dragArmOffsets            = std::to_array<std::int16_t>( {
            10,
            20,
            32,
            46,
            62,
            80,
        } );
        constexpr auto         finalNudges               = std::to_array<Point>( {
            Point{ .x = 0,  .y = 0},
            Point{ .x = 4,  .y = 3},
            Point{.x = -3,  .y = 2},
            Point{ .x = 2, .y = -2},
        } );

        [[nodiscard]]
        std::int32_t
        absolute( std::int32_t value ) noexcept
        {
            return value < 0 ? -value : value;
        }

        [[nodiscard]]
        std::int32_t
        direction_toward( std::int32_t from,
                          std::int32_t to ) noexcept
        {
            return to < from ? -1 : 1;
        }

        [[nodiscard]]
        grab::Result<void>
        move_error( grab::Result<void>& result )
        {
            return std::unexpected( std::move( result.error() ) );
        }

        [[nodiscard]]
        grab::Result<Point>
        translated( Point        origin,
                    std::int32_t dx,
                    std::int32_t dy )
        {
            const std::int32_t x = static_cast<std::int32_t>( origin.x ) + dx;
            const std::int32_t y = static_cast<std::int32_t>( origin.y ) + dy;
            if( x <
                std::numeric_limits<std::int16_t>::min() ||
                x >
                std::numeric_limits<std::int16_t>::max() ||
                y <
                std::numeric_limits<std::int16_t>::min() ||
                y > std::numeric_limits<std::int16_t>::max() )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "Qt drag coordinate is outside int16 range" );
            }

            return Point{
                .x = static_cast<std::int16_t>( x ),
                .y = static_cast<std::int16_t>( y ),
            };
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
        bool
        crosses_start_drag_distance( Point origin,
                                     Point current ) noexcept
        {
            const std::int32_t dx = static_cast<std::int32_t>( current.x ) -
                                    static_cast<std::int32_t>( origin.x );
            const std::int32_t dy = static_cast<std::int32_t>( current.y ) -
                                    static_cast<std::int32_t>( origin.y );
            return absolute( dx ) + absolute( dy ) >= qtStartDragDistance;
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
            auto button_result = button( seat, leftButton, false );
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
        validate_qt_drag_points( Point from,
                                 Point to )
        {
            const std::int32_t x_direction = direction_toward( from.x, to.x );
            for( const std::int16_t arm_offset : dragArmOffsets )
            {
                auto point_result =
                    translated( from,
                                static_cast<std::int32_t>( arm_offset ) * x_direction,
                                armVerticalOffset );
                if( !point_result.has_value() )
                {
                    return std::unexpected( std::move( point_result.error() ) );
                }
            }

            for( const Point nudge : finalNudges )
            {
                auto point_result = translated( to, nudge.x, nudge.y );
                if( !point_result.has_value() )
                {
                    return std::unexpected( std::move( point_result.error() ) );
                }
            }

            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        move_drag_step( Seat&                            seat,
                        Point                            drag_origin,
                        Point                            point,
                        const std::chrono::milliseconds& step_dwell,
                        const std::chrono::milliseconds& drag_start_dwell,
                        bool&                            applied_drag_start_dwell )
        {
            auto move_result = move_and_flush( seat, point );
            if( !move_result.has_value() )
            {
                return move_result;
            }

            if( !applied_drag_start_dwell &&
                crosses_start_drag_distance( drag_origin, point ) )
            {
                std::this_thread::sleep_for( drag_start_dwell );
                applied_drag_start_dwell = true;
            }

            std::this_thread::sleep_for( step_dwell );
            return {};
        }

    }    // namespace

    grab::Result<void>
    qt_drag( Seat&               seat,
             Point               from,
             Point               to,
             const QtDragParams& params )
    {
        if( params.interpolation_steps < minimumInterpolationSteps )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "Qt drag requires at least one interpolation step" );
        }

        auto validation_result = validate_qt_drag_points( from, to );
        if( !validation_result.has_value() )
        {
            return validation_result;
        }

        auto move_result = move_and_flush( seat, from );
        if( !move_result.has_value() )
        {
            return move_result;
        }

        auto press_result = button( seat, leftButton, true );
        if( !press_result.has_value() )
        {
            return press_result;
        }

        const std::int32_t x_direction              = direction_toward( from.x, to.x );
        Point              arm_point                = from;
        bool               applied_drag_start_dwell = false;
        for( const std::int16_t arm_offset : dragArmOffsets )
        {
            auto point_result =
                translated( from,
                            static_cast<std::int32_t>( arm_offset ) * x_direction,
                            armVerticalOffset );
            if( !point_result.has_value() )
            {
                return std::unexpected( std::move( point_result.error() ) );
            }
            arm_point        = *point_result;

            auto step_result = move_drag_step( seat,
                                               from,
                                               arm_point,
                                               params.step_dwell,
                                               params.drag_start_dwell,
                                               applied_drag_start_dwell );
            if( !step_result.has_value() )
            {
                return step_result;
            }
        }

        for( std::int32_t step = 1; step <= params.interpolation_steps; ++step )
        {
            const Point point =
                interpolated( arm_point, to, step, params.interpolation_steps );
            auto step_result = move_drag_step( seat,
                                               from,
                                               point,
                                               params.step_dwell,
                                               params.drag_start_dwell,
                                               applied_drag_start_dwell );
            if( !step_result.has_value() )
            {
                return step_result;
            }
        }

        for( const Point nudge : finalNudges )
        {
            auto point_result = translated( to, nudge.x, nudge.y );
            if( !point_result.has_value() )
            {
                return std::unexpected( std::move( point_result.error() ) );
            }

            auto nudge_result = move_and_flush( seat, *point_result );
            if( !nudge_result.has_value() )
            {
                return nudge_result;
            }
            std::this_thread::sleep_for( params.step_dwell );
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

        auto press_result = button( seat, leftButton, true );
        if( !press_result.has_value() )
        {
            return press_result;
        }

        return release_and_flush( seat );
    }

}    // namespace grab::input
