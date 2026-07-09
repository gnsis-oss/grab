#pragma once

#include <cmath>
#include <cstdint>

namespace grab::geometry
{

    namespace detail
    {

        constexpr double       half_fraction = 0.5;
        constexpr std::int64_t zero_long     = 0;
        constexpr std::int64_t round_step    = 1;
        constexpr std::int64_t even_divisor  = 2;

        [[nodiscard]]
        constexpr std::int32_t
        to_coord( std::int64_t value ) noexcept
        {
            return static_cast<std::int32_t>( value );
        }

    }    // namespace detail

    [[nodiscard]]
    inline std::int32_t
    round_half_even( double value ) noexcept
    {
        const auto lower    = static_cast<std::int64_t>( std::floor( value ) );
        const auto fraction = value - static_cast<double>( lower );

        if( fraction < detail::half_fraction )
        {
            return detail::to_coord( lower );
        }
        if( fraction > detail::half_fraction )
        {
            return detail::to_coord( lower + detail::round_step );
        }
        if( ( lower % detail::even_divisor ) == detail::zero_long )
        {
            return detail::to_coord( lower );
        }
        return detail::to_coord( lower + detail::round_step );
    }

    [[nodiscard]]
    constexpr std::int64_t
    floor_div( std::int64_t numerator,
               std::int64_t denominator ) noexcept
    {
        const auto quotient  = numerator / denominator;
        const auto remainder = numerator % denominator;

        if( remainder == detail::zero_long )
        {
            return quotient;
        }

        const auto same_sign =
            ( remainder > detail::zero_long ) == ( denominator > detail::zero_long );
        if( same_sign )
        {
            return quotient;
        }
        return quotient - detail::round_step;
    }

    struct Point
    {
            std::int32_t x = 0;
            std::int32_t y = 0;

            [[nodiscard]]
            constexpr Point
            translated( std::int32_t dx,
                        std::int32_t dy ) const noexcept
            {
                return Point{
                    .x = detail::to_coord( static_cast<std::int64_t>( x ) +
                                           static_cast<std::int64_t>( dx ) ),
                    .y = detail::to_coord( static_cast<std::int64_t>( y ) +
                                           static_cast<std::int64_t>( dy ) ),
                };
            }

            [[nodiscard]]
            friend constexpr Point
            operator+( Point lhs,
                       Point rhs ) noexcept
            {
                return lhs.translated( rhs.x, rhs.y );
            }

            [[nodiscard]]
            friend constexpr Point
            operator-( Point lhs,
                       Point rhs ) noexcept
            {
                return lhs.translated( -rhs.x, -rhs.y );
            }

            [[nodiscard]]
            friend constexpr bool
            operator==( Point lhs,
                        Point rhs ) noexcept = default;
    };

    struct PointF
    {
            double x = 0.0;
            double y = 0.0;

            [[nodiscard]]
            constexpr PointF
            translated( double dx,
                        double dy ) const noexcept
            {
                return PointF{
                    .x = x + dx,
                    .y = y + dy,
                };
            }

            [[nodiscard]]
            constexpr PointF
            scaled( double value ) const noexcept
            {
                return PointF{
                    .x = x * value,
                    .y = y * value,
                };
            }

            [[nodiscard]]
            Point
            to_point() const noexcept
            {
                return Point{
                    .x = round_half_even( x ),
                    .y = round_half_even( y ),
                };
            }

            [[nodiscard]]
            static constexpr PointF
            lerp( PointF lhs,
                  PointF rhs,
                  double t ) noexcept
            {
                return PointF{
                    .x = lhs.x + ( ( rhs.x - lhs.x ) * t ),
                    .y = lhs.y + ( ( rhs.y - lhs.y ) * t ),
                };
            }

            [[nodiscard]]
            friend constexpr PointF
            operator+( PointF lhs,
                       PointF rhs ) noexcept
            {
                return lhs.translated( rhs.x, rhs.y );
            }

            [[nodiscard]]
            friend constexpr PointF
            operator-( PointF lhs,
                       PointF rhs ) noexcept
            {
                return lhs.translated( -rhs.x, -rhs.y );
            }

            [[nodiscard]]
            friend constexpr bool
            operator==( PointF lhs,
                        PointF rhs ) noexcept = default;
    };

}    // namespace grab::geometry
