#include "grab/result.hpp"
#include "grab/space.hpp"
#include "kernel/presentation/space_graph.hpp"

#include <gtest/gtest.h>

namespace
{

    constexpr double windowScale      = 2.0;
    constexpr double windowTranslateX = 100.0;
    constexpr double sourceCoordinate = 10.0;
    constexpr double expectedMappedX  = 120.0;
    constexpr double expectedMappedY  = 20.0;

}    // namespace

TEST( SpaceGraph,
      ComposesTransformsAndScales )
{
    grab::detail::SpaceGraph graph;
    const auto               window = graph.add_space();
    const auto               output = graph.add_space();
    const auto               global = graph.add_space();

    graph.add_transform( {
        .source      = window,
        .destination = output,
        .map =
            {
                  .xx = windowScale,
                  .tx = windowTranslateX,
                  .yy = windowScale,
                  },
        .trust = grab::TransformTrust::Exact,
    } );
    graph.add_transform( {
        .source      = output,
        .destination = global,
        .trust       = grab::TransformTrust::Calibrated,
    } );

    const auto point =
        graph.map( { .x = sourceCoordinate, .y = sourceCoordinate, .space = window },
                   global );
    ASSERT_TRUE( point.has_value() );
    EXPECT_DOUBLE_EQ( point->x, expectedMappedX );
    EXPECT_DOUBLE_EQ( point->y, expectedMappedY );
    EXPECT_EQ( point->space, global );

    const auto trust = graph.route_trust( window, global );
    ASSERT_TRUE( trust.has_value() );
    EXPECT_EQ( *trust, grab::TransformTrust::Calibrated );
}

TEST( SpaceGraph,
      NoRouteIsTyped )
{
    grab::detail::SpaceGraph graph;
    const auto               source      = graph.add_space();
    const auto               destination = graph.add_space();

    const auto               result =
        graph.map( { .x = 0.0, .y = 0.0, .space = source }, destination );
    ASSERT_FALSE( result.has_value() );
    EXPECT_EQ( result.error().code, grab::ErrorCode::RouteUnavailable );
}

TEST( SpaceGraph,
      StaleRouteReportsTopologyChanged )
{
    grab::detail::SpaceGraph graph;
    const auto               source      = graph.add_space();
    const auto               destination = graph.add_space();
    graph.add_transform( {
        .source      = source,
        .destination = destination,
        .trust       = grab::TransformTrust::Exact,
    } );

    graph.bump_generation( destination );

    const auto result =
        graph.map( { .x = 0.0, .y = 0.0, .space = source }, destination );
    ASSERT_FALSE( result.has_value() );
    EXPECT_EQ( result.error().code, grab::ErrorCode::TopologyChanged );
}
