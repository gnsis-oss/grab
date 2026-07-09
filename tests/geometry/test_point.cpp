#include "grab/geometry/point.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <array>
#include <cmath>
#include <cstdint>
// clang-format on

// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

namespace
{

    namespace geometry                   = grab::geometry;

    constexpr double       half_fraction = 0.5;
    constexpr std::int64_t zero_long     = 0;
    constexpr std::int64_t round_step    = 1;
    constexpr std::int64_t even_divisor  = 2;

    struct RoundCase
    {
            double       value    = 0.0;
            std::int32_t expected = 0;
    };

    struct DivCase
    {
            std::int64_t numerator   = 0;
            std::int64_t denominator = 1;
            std::int64_t expected    = 0;
    };

    [[nodiscard]]
    std::int32_t
    reference_round_half_even( double value ) noexcept
    {
        const auto lower    = static_cast<std::int64_t>( std::floor( value ) );
        const auto fraction = value - static_cast<double>( lower );

        if( fraction < half_fraction )
        {
            return static_cast<std::int32_t>( lower );
        }
        if( fraction > half_fraction )
        {
            return static_cast<std::int32_t>( lower + round_step );
        }
        if( ( lower % even_divisor ) == zero_long )
        {
            return static_cast<std::int32_t>( lower );
        }
        return static_cast<std::int32_t>( lower + round_step );
    }

    [[nodiscard]]
    std::int64_t
    reference_floor_div( std::int64_t numerator,
                         std::int64_t denominator ) noexcept
    {
        const auto quotient  = numerator / denominator;
        const auto remainder = numerator % denominator;

        if( remainder == zero_long )
        {
            return quotient;
        }

        const auto same_sign = ( remainder > zero_long ) == ( denominator > zero_long );
        if( same_sign )
        {
            return quotient;
        }
        return quotient - round_step;
    }

}    // namespace

TEST( GeometryPoint,
      RoundHalfEvenMatchesGestureHelperParity )
{
    constexpr std::array<RoundCase, 14U> cases{
        RoundCase{ .value = 0.0,  .expected = 0},
        RoundCase{ .value = 0.5,  .expected = 0},
        RoundCase{ .value = 1.5,  .expected = 2},
        RoundCase{ .value = 2.5,  .expected = 2},
        RoundCase{ .value = 3.5,  .expected = 4},
        RoundCase{.value = -0.5,  .expected = 0},
        RoundCase{.value = -1.5, .expected = -2},
        RoundCase{.value = -2.5, .expected = -2},
        RoundCase{.value = -3.5, .expected = -4},
        RoundCase{ .value = 3.4,  .expected = 3},
        RoundCase{ .value = 3.6,  .expected = 4},
        RoundCase{.value = -3.4, .expected = -3},
        RoundCase{.value = -3.6, .expected = -4},
        RoundCase{.value = 50.5, .expected = 50},
    };

    for( const RoundCase& current : cases )
    {
        EXPECT_EQ( reference_round_half_even( current.value ), current.expected );
        EXPECT_EQ( geometry::round_half_even( current.value ),
                   reference_round_half_even( current.value ) );
    }
}

TEST( GeometryPoint,
      FloorDivMatchesGestureHelperParity )
{
    constexpr std::array<DivCase, 12U> cases{
        DivCase{ .numerator = 5,  .denominator = 2,  .expected = 2},
        DivCase{.numerator = -5,  .denominator = 2, .expected = -3},
        DivCase{ .numerator = 5, .denominator = -2, .expected = -3},
        DivCase{.numerator = -5, .denominator = -2,  .expected = 2},
        DivCase{ .numerator = 4,  .denominator = 2,  .expected = 2},
        DivCase{.numerator = -4,  .denominator = 2, .expected = -2},
        DivCase{ .numerator = 4, .denominator = -2, .expected = -2},
        DivCase{.numerator = -4, .denominator = -2,  .expected = 2},
        DivCase{ .numerator = 0,  .denominator = 2,  .expected = 0},
        DivCase{ .numerator = 1, .denominator = 16,  .expected = 0},
        DivCase{.numerator = -1, .denominator = 16, .expected = -1},
        DivCase{.numerator = 47, .denominator = 16,  .expected = 2},
    };

    for( const DivCase& current : cases )
    {
        EXPECT_EQ( reference_floor_div( current.numerator, current.denominator ),
                   current.expected );
        EXPECT_EQ( geometry::floor_div( current.numerator, current.denominator ),
                   reference_floor_div( current.numerator, current.denominator ) );
    }
}

TEST( GeometryPoint,
      PointArithmeticTranslatesAndCompares )
{
    constexpr geometry::Point point{
        .x = 12,
        .y = -7,
    };
    constexpr geometry::Point delta{
        .x = -5,
        .y = 9,
    };
    constexpr geometry::Point expected_sum{
        .x = 7,
        .y = 2,
    };
    constexpr geometry::Point expected_difference{
        .x = 17,
        .y = -16,
    };
    constexpr geometry::Point expected_translation{
        .x = 15,
        .y = -11,
    };

    EXPECT_EQ( point + delta, expected_sum );
    EXPECT_EQ( point - delta, expected_difference );
    EXPECT_EQ( point.translated( 3, -4 ), expected_translation );
}

TEST( GeometryPoint,
      PointFScalesLerpsAndRoundsToPoint )
{
    constexpr geometry::PointF first{
        .x = 2.0,
        .y = -6.0,
    };
    constexpr geometry::PointF second{
        .x = 10.0,
        .y = 2.0,
    };
    constexpr geometry::PointF expected_scaled{
        .x = 5.0,
        .y = -15.0,
    };
    constexpr geometry::PointF expected_lerp{
        .x = 4.0,
        .y = -4.0,
    };
    const geometry::Point expected_rounded{
        .x = 2,
        .y = -2,
    };

    EXPECT_EQ( first.scaled( 2.5 ), expected_scaled );
    EXPECT_EQ( geometry::PointF::lerp( first, second, 0.25 ), expected_lerp );
    EXPECT_EQ( ( geometry::PointF{ .x = 2.5, .y = -2.5 } ).to_point(),
               expected_rounded );
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
