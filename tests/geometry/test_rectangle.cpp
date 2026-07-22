#include "grab/geometry/point.hpp"
#include "grab/geometry/rectangle.hpp"
#include "grab/geometry/size.hpp"

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

    struct FractionCase
    {
            geometry::Rectangle rect;
            double              fx = 0.0;
            double              fy = 0.0;
            geometry::Point     expected;
    };

    struct OffsetCase
    {
            geometry::Rectangle rect;
            std::int32_t        ox = 0;
            std::int32_t        oy = 0;
            geometry::Point     expected;
    };

    [[nodiscard]]
    std::int64_t
    reference_round_half_even( double value ) noexcept
    {
        const auto lower    = static_cast<std::int64_t>( std::floor( value ) );
        const auto fraction = value - static_cast<double>( lower );

        if( fraction < half_fraction )
        {
            return lower;
        }
        if( fraction > half_fraction )
        {
            return lower + round_step;
        }
        if( ( lower % even_divisor ) == zero_long )
        {
            return lower;
        }
        return lower + round_step;
    }

    [[nodiscard]]
    geometry::Point
    reference_frac_point( const geometry::Rectangle& rect,
                          double                     fx,
                          double                     fy ) noexcept
    {
        return geometry::Point{
            .x = static_cast<std::int32_t>(
                static_cast<std::int64_t>( rect.x ) +
                reference_round_half_even( static_cast<double>( rect.width ) * fx )
            ),
            .y = static_cast<std::int32_t>(
                static_cast<std::int64_t>( rect.y ) +
                reference_round_half_even( static_cast<double>( rect.height ) * fy )
            ),
        };
    }

    [[nodiscard]]
    constexpr geometry::Point
    reference_off_point( const geometry::Rectangle& rect,
                         std::int32_t               ox,
                         std::int32_t               oy ) noexcept
    {
        return geometry::Point{
            .x = static_cast<std::int32_t>( static_cast<std::int64_t>( rect.x ) +
                                            static_cast<std::int64_t>( ox ) ),
            .y = static_cast<std::int32_t>( static_cast<std::int64_t>( rect.y ) +
                                            static_cast<std::int64_t>( oy ) ),
        };
    }

}    // namespace

TEST( GeometryRectangle,
      FractionPointMatchesGestureHelperParity )
{
    constexpr std::array<FractionCase, 5U> cases{
        FractionCase{
                     .rect =
                     geometry::Rectangle{
                     .x      = 10,
                     .y      = 20,
                     .width  = 101U,
                     .height = 51U,
                     },   .fx       = 0.5,
                     .fy       = 0.5,
                     .expected = geometry::Point{ .x = 60, .y = 46 },
                     },
        FractionCase{
                     .rect =
                     geometry::Rectangle{
                     .x      = -10,
                     .y      = -20,
                     .width  = 11U,
                     .height = 9U,
                     },   .fx       = 0.5,
                     .fy       = 0.5,
                     .expected = geometry::Point{ .x = -4, .y = -16 },
                     },
        FractionCase{
                     .rect =
                     geometry::Rectangle{
                     .x      = 0,
                     .y      = 0,
                     .width  = 5U,
                     .height = 3U,
                     },   .fx       = 0.5,
                     .fy       = 0.5,
                     .expected = geometry::Point{ .x = 2, .y = 2 },
                     },
        FractionCase{
                     .rect =
                     geometry::Rectangle{
                     .x      = 100,
                     .y      = 200,
                     .width  = 1'000U,
                     .height = 1'000U,
                     }, .fx       = 0.330,
                     .fy       = 0.1007,
                     .expected = geometry::Point{ .x = 430, .y = 301 },
                     },
        FractionCase{
                     .rect =
                     geometry::Rectangle{
                     .x      = 5,
                     .y      = 7,
                     .width  = 10U,
                     .height = 10U,
                     },   .fx       = 1.0,
                     .fy       = 0.0,
                     .expected = geometry::Point{ .x = 15, .y = 7 },
                     },
    };

    for( const FractionCase& current : cases )
    {
        EXPECT_EQ( reference_frac_point( current.rect, current.fx, current.fy ),
                   current.expected );
        EXPECT_EQ( current.rect.point_at_fraction( current.fx, current.fy ),
                   reference_frac_point( current.rect, current.fx, current.fy ) );
    }
}

TEST( GeometryRectangle,
      OffsetPointMatchesGestureHelperParity )
{
    constexpr std::array<OffsetCase, 4U> cases{
        OffsetCase{
                   .rect =
                   geometry::Rectangle{
                   .x      = 10,
                   .y      = 20,
                   .width  = 101U,
                   .height = 51U,
                   },  .ox       = 3,
                   .oy       = -4,
                   .expected = geometry::Point{ .x = 13, .y = 16 },
                   },
        OffsetCase{
                   .rect =
                   geometry::Rectangle{
                   .x      = -10,
                   .y      = -20,
                   .width  = 11U,
                   .height = 9U,
                   }, .ox       = -7,
                   .oy       = 8,
                   .expected = geometry::Point{ .x = -17, .y = -12 },
                   },
        OffsetCase{
                   .rect =
                   geometry::Rectangle{
                   .x      = 0,
                   .y      = 0,
                   .width  = 5U,
                   .height = 3U,
                   },  .ox       = 0,
                   .oy       = 0,
                   .expected = geometry::Point{ .x = 0, .y = 0 },
                   },
        OffsetCase{
                   .rect =
                   geometry::Rectangle{
                   .x      = 5,
                   .y      = 7,
                   .width  = 10U,
                   .height = 10U,
                   }, .ox       = 10,
                   .oy       = 10,
                   .expected = geometry::Point{ .x = 15, .y = 17 },
                   },
    };

    for( const OffsetCase& current : cases )
    {
        EXPECT_EQ( reference_off_point( current.rect, current.ox, current.oy ),
                   current.expected );
        EXPECT_EQ( current.rect.point_at_offset( current.ox, current.oy ),
                   reference_off_point( current.rect, current.ox, current.oy ) );
    }
}

TEST( GeometryRectangle,
      ReportsDerivedGeometry )
{
    constexpr geometry::Rectangle rect{
        .x      = 10,
        .y      = 20,
        .width  = 101U,
        .height = 51U,
    };
    constexpr std::array<geometry::Point, 4U> expected_corners{
        geometry::Point{ .x = 10, .y = 20},
        geometry::Point{.x = 111, .y = 20},
        geometry::Point{.x = 111, .y = 71},
        geometry::Point{ .x = 10, .y = 71},
    };
    constexpr geometry::Point expected_origin{
        .x = 10,
        .y = 20,
    };
    constexpr geometry::Size expected_size{
        .width  = 101U,
        .height = 51U,
    };
    constexpr geometry::Point expected_center{
        .x = 60,
        .y = 46,
    };

    EXPECT_EQ( rect.origin(), expected_origin );
    EXPECT_EQ( rect.size(), expected_size );
    EXPECT_EQ( rect.center(), expected_center );
    EXPECT_EQ( rect.right(), 111 );
    EXPECT_EQ( rect.bottom(), 71 );
    EXPECT_TRUE( rect.contains( geometry::Point{ .x = 10, .y = 20 } ) );
    EXPECT_TRUE( rect.contains( geometry::Point{ .x = 110, .y = 70 } ) );
    EXPECT_FALSE( rect.contains( geometry::Point{ .x = 111, .y = 70 } ) );
    EXPECT_FALSE( rect.contains( geometry::Point{ .x = 110, .y = 71 } ) );
    EXPECT_EQ( rect.corners(), expected_corners );
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
