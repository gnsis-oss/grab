// Contract tests for Kahn's algorithm over an AdjacencyGraph keyed by StepId.
//
// Three properties matter to a sequence player and nothing else does: a
// linearisable graph puts every predecessor before its successors, a cycle is
// reported rather than silently truncated, and an empty document is a valid
// document with nothing to run — not an error.

#include "grab/sequence_types.hpp"
#include "kernel/graph/topological_order.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstddef>
#include <optional>
#include <vector>
// clang-format on

namespace
{

    using grab::sequence::DependencyEdge;
    using grab::sequence::StepId;

    using Graph = grab::kernel::AdjacencyGraph<StepId, DependencyEdge>;

    constexpr StepId::Half firstGeneration = 1U;
    constexpr StepId::Half indexA          = 0U;
    constexpr StepId::Half indexB          = 1U;
    constexpr StepId::Half indexC          = 2U;
    constexpr StepId::Half indexD          = 3U;
    constexpr StepId::Half indexE          = 4U;

    constexpr StepId       stepA{ indexA, firstGeneration };
    constexpr StepId       stepB{ indexB, firstGeneration };
    constexpr StepId       stepC{ indexC, firstGeneration };
    constexpr StepId       stepD{ indexD, firstGeneration };
    constexpr StepId       stepE{ indexE, firstGeneration };

    constexpr std::size_t  diamondNodeCount = 4U;
    constexpr std::size_t  tripleNodeCount  = 3U;
    constexpr std::size_t  singleNodeCount  = 1U;
    constexpr std::size_t  emptyNodeCount   = 0U;

    // order.size() when the id is absent, which is never a valid position.
    [[nodiscard]]
    std::size_t
    position_of( const std::vector<StepId>& order,
                 StepId                     id )
    {
        for( std::size_t index = 0U; index < order.size(); ++index )
        {
            if( order[index] == id )
            {
                return index;
            }
        }
        return order.size();
    }

    // A -> B -> D, A -> C -> D.
    [[nodiscard]]
    Graph
    make_diamond()
    {
        Graph graph;
        EXPECT_TRUE( graph.add_node( stepA ) );
        EXPECT_TRUE( graph.add_node( stepB ) );
        EXPECT_TRUE( graph.add_node( stepC ) );
        EXPECT_TRUE( graph.add_node( stepD ) );
        EXPECT_TRUE( graph.add_edge( stepA, stepB, DependencyEdge{} ) );
        EXPECT_TRUE( graph.add_edge( stepA, stepC, DependencyEdge{} ) );
        EXPECT_TRUE( graph.add_edge( stepB, stepD, DependencyEdge{} ) );
        EXPECT_TRUE( graph.add_edge( stepC, stepD, DependencyEdge{} ) );
        return graph;
    }

}    // namespace

TEST( TopologicalOrder,
      DiamondPlacesEveryPredecessorBeforeItsSuccessors )
{
    const Graph graph = make_diamond();
    ASSERT_EQ( graph.node_count(), diamondNodeCount );

    const auto order = grab::kernel::topological_order( graph );
    ASSERT_TRUE( order.has_value() );
    ASSERT_EQ( order->size(), diamondNodeCount );

    const std::size_t at_a = position_of( *order, stepA );
    const std::size_t at_b = position_of( *order, stepB );
    const std::size_t at_c = position_of( *order, stepC );
    const std::size_t at_d = position_of( *order, stepD );

    ASSERT_LT( at_a, order->size() );
    ASSERT_LT( at_b, order->size() );
    ASSERT_LT( at_c, order->size() );
    ASSERT_LT( at_d, order->size() );

    EXPECT_LT( at_a, at_b );
    EXPECT_LT( at_a, at_c );
    EXPECT_LT( at_b, at_d );
    EXPECT_LT( at_c, at_d );
}

TEST( TopologicalOrder,
      IsDeterministicForTheSameGraph )
{
    const Graph graph  = make_diamond();

    const auto  first  = grab::kernel::topological_order( graph );
    const auto  second = grab::kernel::topological_order( graph );

    ASSERT_TRUE( first.has_value() );
    ASSERT_TRUE( second.has_value() );
    EXPECT_EQ( *first, *second );

    // nodes() walks a std::map, so the seed set is appended in ascending key
    // order and the root of a diamond comes first.
    ASSERT_FALSE( first->empty() );
    EXPECT_EQ( first->front(), stepA );
}

TEST( TopologicalOrder,
      ThreeCycleReturnsNullopt )
{
    Graph graph;
    ASSERT_TRUE( graph.add_node( stepA ) );
    ASSERT_TRUE( graph.add_node( stepB ) );
    ASSERT_TRUE( graph.add_node( stepC ) );
    ASSERT_TRUE( graph.add_edge( stepA, stepB, DependencyEdge{} ) );
    ASSERT_TRUE( graph.add_edge( stepB, stepC, DependencyEdge{} ) );
    ASSERT_TRUE( graph.add_edge( stepC, stepA, DependencyEdge{} ) );
    ASSERT_EQ( graph.node_count(), tripleNodeCount );

    const auto order = grab::kernel::topological_order( graph );
    EXPECT_FALSE( order.has_value() );
}

TEST( TopologicalOrder,
      ACycleHiddenBehindALinearisablePrefixIsStillReported )
{
    // A -> B, plus a disjoint two-cycle. A partial linearisation exists, so an
    // implementation that stopped at "made some progress" would pass this
    // graph.
    Graph graph;
    ASSERT_TRUE( graph.add_node( stepA ) );
    ASSERT_TRUE( graph.add_node( stepB ) );
    ASSERT_TRUE( graph.add_node( stepC ) );
    ASSERT_TRUE( graph.add_node( stepD ) );
    ASSERT_TRUE( graph.add_edge( stepA, stepB, DependencyEdge{} ) );
    ASSERT_TRUE( graph.add_edge( stepC, stepD, DependencyEdge{} ) );
    ASSERT_TRUE( graph.add_edge( stepD, stepC, DependencyEdge{} ) );

    const auto order = grab::kernel::topological_order( graph );
    EXPECT_FALSE( order.has_value() );
}

TEST( TopologicalOrder,
      EmptyGraphYieldsAnEmptyOrderRatherThanAnError )
{
    const Graph graph;
    ASSERT_EQ( graph.node_count(), emptyNodeCount );

    const auto order = grab::kernel::topological_order( graph );

    // An empty document is a valid document with nothing to run. nullopt here
    // would make "no steps" indistinguishable from "cyclic".
    ASSERT_TRUE( order.has_value() );
    EXPECT_TRUE( order->empty() );
}

TEST( TopologicalOrder,
      DisconnectedNodesAllAppearExactlyOnce )
{
    Graph graph;
    ASSERT_TRUE( graph.add_node( stepA ) );
    ASSERT_TRUE( graph.add_node( stepB ) );
    ASSERT_TRUE( graph.add_node( stepC ) );

    const auto order = grab::kernel::topological_order( graph );
    ASSERT_TRUE( order.has_value() );
    ASSERT_EQ( order->size(), tripleNodeCount );
    EXPECT_LT( position_of( *order, stepA ), order->size() );
    EXPECT_LT( position_of( *order, stepB ), order->size() );
    EXPECT_LT( position_of( *order, stepC ), order->size() );
}

TEST( TopologicalOrder,
      CannotSeeASelfEdgeBecauseTheGraphRejectsIt )
{
    // Documented contract: add_edge returns false for source == target, so the
    // edge never enters the graph and the sort succeeds on the graph minus the
    // rejected edge. Rejecting a self-dependency is the loader's job, at the
    // point where add_edge answers false.
    Graph graph;
    ASSERT_TRUE( graph.add_node( stepE ) );
    EXPECT_FALSE( graph.add_edge( stepE, stepE, DependencyEdge{} ) );

    const auto order = grab::kernel::topological_order( graph );
    ASSERT_TRUE( order.has_value() );
    ASSERT_EQ( order->size(), singleNodeCount );
    EXPECT_EQ( order->front(), stepE );
}
