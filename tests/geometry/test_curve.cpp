#include "grab/geometry/curve.hpp"
#include "grab/geometry/point.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <vector>
// clang-format on

// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

namespace
{

    namespace geometry = grab::geometry;

    void
    expect_point_f( geometry::PointF point,
                    double           x,
                    double           y )
    {
        EXPECT_DOUBLE_EQ( point.x, x );
        EXPECT_DOUBLE_EQ( point.y, y );
    }

}    // namespace

TEST( GeometryCurve,
      ReportsDegreeAndEvaluatesEndpoints )
{
    const geometry::Curve curve{
        .control = {
                    geometry::PointF{ .x = 0.0, .y = 0.0 },
                    geometry::PointF{ .x = 1.0, .y = 2.0 },
                    geometry::PointF{ .x = 2.0, .y = 0.0 },
                    },
    };

    EXPECT_EQ( curve.degree(), 2U );
    expect_point_f( curve.evaluate( 0.0 ), 0.0, 0.0 );
    expect_point_f( curve.evaluate( 0.5 ), 1.0, 1.0 );
    expect_point_f( curve.evaluate( 1.0 ), 2.0, 0.0 );
}

TEST( GeometryCurve,
      BuildsLineAndCubicFactories )
{
    const auto line = geometry::Curve::line( geometry::PointF{ .x = 0.0, .y = 1.0 },
                                             geometry::PointF{ .x = 10.0, .y = 11.0 } );
    EXPECT_EQ( line.degree(), 1U );
    ASSERT_EQ( line.control.size(), 2U );
    expect_point_f( line.control.at( 0U ), 0.0, 1.0 );
    expect_point_f( line.control.at( 1U ), 10.0, 11.0 );

    const auto cubic = geometry::Curve::cubic( geometry::PointF{ .x = 0.0, .y = 0.0 },
                                               geometry::PointF{ .x = 1.0, .y = 2.0 },
                                               geometry::PointF{ .x = 3.0, .y = 4.0 },
                                               geometry::PointF{ .x = 5.0, .y = 6.0 } );
    EXPECT_EQ( cubic.degree(), 3U );
    ASSERT_EQ( cubic.control.size(), 4U );
    expect_point_f( cubic.evaluate( 0.0 ), 0.0, 0.0 );
    expect_point_f( cubic.evaluate( 1.0 ), 5.0, 6.0 );
}

TEST( GeometryCurve,
      SamplesCountAndRoundsWithHalfEven )
{
    const auto line = geometry::Curve::line( geometry::PointF{ .x = 0.0, .y = 0.0 },
                                             geometry::PointF{ .x = 5.0, .y = 5.0 } );
    const std::vector<geometry::Point> expected{
        geometry::Point{.x = 0, .y = 0},
        geometry::Point{.x = 2, .y = 2},
        geometry::Point{.x = 5, .y = 5},
    };
    const std::vector<geometry::Point> single_sample{
        geometry::Point{ .x = 0, .y = 0 },
    };

    EXPECT_TRUE( line.sample( 0U ).empty() );
    EXPECT_EQ( line.sample( 1U ), single_sample );
    EXPECT_EQ( line.sample( 3U ), expected );
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
