#include "kernel/graph/adjacency_graph.hpp"
#include "kernel/graph/graph_delta.hpp"
#include "kernel/graph/graph_traversal.hpp"

#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <utility>
#include <vector>

namespace
{

    using Graph = grab::kernel::AdjacencyGraph<std::uint64_t, std::uint32_t>;

    constexpr std::uint64_t nodeA         = 1U;
    constexpr std::uint64_t nodeB         = 2U;
    constexpr std::uint64_t nodeC         = 3U;
    constexpr std::uint64_t nodeD         = 4U;
    constexpr std::uint64_t absentNode    = 99U;

    constexpr std::uint32_t firstPayload  = 0X01U;
    constexpr std::uint32_t secondPayload = 0X02U;

    // The two endpoint pairs that a hash fold of the form
    // `(hash(from) * 1'000'003) ^ hash(to)` maps onto the same bucket. Keeping
    // them named here is the regression: an edge delta keyed by such a fold
    // silently drops one of the two, and grab derives widget events from that
    // delta, so a dropped edge is a missing event.
    constexpr std::uint64_t collidingFrom = 3U;
    constexpr std::uint64_t collidingTo   = 2'262'152U;

    constexpr std::size_t   noEntries     = 0U;
    constexpr std::size_t   oneEntry      = 1U;
    constexpr std::size_t   twoEntries    = 2U;
    constexpr std::size_t   threeEntries  = 3U;

    // Records the order nodes and edges are handed to it.
    class RecordingVisitor final
    {
        public:

            void
            visit_node( std::uint64_t node )
            {
                nodes.push_back( node );
            }

            void
            visit_edge( std::uint64_t source,
                        std::uint64_t target )
            {
                edges.emplace_back( source, target );
            }

            std::vector<std::uint64_t>                           nodes;
            std::vector<std::pair<std::uint64_t, std::uint64_t>> edges;
    };

    // A visitor with no visit_edge hook at all, to pin that the hook is
    // genuinely optional rather than merely defaulted.
    class NodeOnlyVisitor final
    {
        public:

            void
            visit_node( std::uint64_t node )
            {
                nodes.push_back( node );
            }

            std::vector<std::uint64_t> nodes;
    };

    [[nodiscard]]
    Graph
    chain()
    {
        Graph graph;
        EXPECT_TRUE( graph.add_node( nodeA ) );
        EXPECT_TRUE( graph.add_node( nodeB ) );
        EXPECT_TRUE( graph.add_node( nodeC ) );
        EXPECT_TRUE( graph.add_edge( nodeA, nodeB, firstPayload ) );
        EXPECT_TRUE( graph.add_edge( nodeB, nodeC, firstPayload ) );
        return graph;
    }

}    // namespace

TEST( AdjacencyGraph,
      RejectsDuplicateNodesAndEdges )
{
    Graph graph;
    EXPECT_TRUE( graph.add_node( nodeA ) );
    EXPECT_FALSE( graph.add_node( nodeA ) );
    EXPECT_TRUE( graph.add_node( nodeB ) );
    EXPECT_TRUE( graph.add_edge( nodeA, nodeB, firstPayload ) );
    EXPECT_FALSE( graph.add_edge( nodeA, nodeB, secondPayload ) );
    EXPECT_EQ( graph.node_count(), twoEntries );
    EXPECT_EQ( graph.edge_count(), oneEntry );
}

TEST( AdjacencyGraph,
      RejectsEdgesWithAnUnknownEndpointOrToItself )
{
    Graph graph;
    EXPECT_TRUE( graph.add_node( nodeA ) );
    EXPECT_FALSE( graph.add_edge( nodeA, absentNode, firstPayload ) );
    EXPECT_FALSE( graph.add_edge( absentNode, nodeA, firstPayload ) );
    EXPECT_FALSE( graph.add_edge( nodeA, nodeA, firstPayload ) );
    EXPECT_EQ( graph.edge_count(), noEntries );
}

TEST( AdjacencyGraph,
      KeepsAdjacencyListsSortedByTarget )
{
    Graph graph;
    EXPECT_TRUE( graph.add_node( nodeA ) );
    EXPECT_TRUE( graph.add_node( nodeB ) );
    EXPECT_TRUE( graph.add_node( nodeC ) );
    EXPECT_TRUE( graph.add_node( nodeD ) );
    // Inserted out of order on purpose.
    EXPECT_TRUE( graph.add_edge( nodeA, nodeD, firstPayload ) );
    EXPECT_TRUE( graph.add_edge( nodeA, nodeB, firstPayload ) );
    EXPECT_TRUE( graph.add_edge( nodeA, nodeC, firstPayload ) );

    const auto edges = graph.out_edges( nodeA );
    ASSERT_EQ( edges.size(), threeEntries );
    EXPECT_EQ( edges[0].target, nodeB );
    EXPECT_EQ( edges[1].target, nodeC );
    EXPECT_EQ( edges[2].target, nodeD );
}

TEST( AdjacencyGraph,
      TracksIncomingEdgesAlongsideOutgoing )
{
    const auto graph    = chain();

    const auto incoming = graph.in_edges( nodeB );
    ASSERT_EQ( incoming.size(), oneEntry );
    EXPECT_EQ( incoming[0].target, nodeA );
    EXPECT_TRUE( graph.in_edges( nodeA ).empty() );
}

TEST( AdjacencyGraph,
      RemovingANodeRemovesEveryEdgeTouchingIt )
{
    auto graph = chain();
    EXPECT_TRUE( graph.remove_node( nodeB ) );

    EXPECT_FALSE( graph.contains_node( nodeB ) );
    EXPECT_TRUE( graph.out_edges( nodeA ).empty() );
    EXPECT_TRUE( graph.in_edges( nodeC ).empty() );
    EXPECT_EQ( graph.edge_count(), noEntries );
    EXPECT_FALSE( graph.remove_node( nodeB ) );
}

TEST( AdjacencyGraph,
      RemovingAnEdgeClearsBothDirections )
{
    auto graph = chain();
    EXPECT_TRUE( graph.remove_edge( nodeA, nodeB ) );

    EXPECT_FALSE( graph.contains_edge( nodeA, nodeB ) );
    EXPECT_TRUE( graph.in_edges( nodeB ).empty() );
    EXPECT_TRUE( graph.contains_node( nodeA ) );
    EXPECT_FALSE( graph.remove_edge( nodeA, nodeB ) );
}

TEST( AdjacencyGraph,
      EdgePayloadIsNullWhenTheEdgeIsAbsent )
{
    const auto  graph   = chain();
    const auto* payload = graph.edge_payload( nodeA, nodeB );
    ASSERT_NE( payload, nullptr );
    EXPECT_EQ( *payload, firstPayload );
    EXPECT_EQ( graph.edge_payload( nodeA, nodeC ), nullptr );
    EXPECT_EQ( graph.edge_payload( absentNode, nodeA ), nullptr );
}

TEST( GraphTraversal,
      BreadthFirstVisitsEveryReachableNodeOnce )
{
    const auto       graph = chain();
    RecordingVisitor visitor;
    grab::kernel::breadth_first_search( graph, nodeA, visitor );

    const std::vector<std::uint64_t> expected{ nodeA, nodeB, nodeC };
    EXPECT_EQ( visitor.nodes, expected );
}

TEST( GraphTraversal,
      BreadthFirstOffersEveryEdgeItExamines )
{
    const auto       graph = chain();
    RecordingVisitor visitor;
    grab::kernel::breadth_first_search( graph, nodeA, visitor );

    ASSERT_EQ( visitor.edges.size(), twoEntries );
    EXPECT_EQ( visitor.edges[0].first, nodeA );
    EXPECT_EQ( visitor.edges[0].second, nodeB );
    EXPECT_EQ( visitor.edges[1].first, nodeB );
    EXPECT_EQ( visitor.edges[1].second, nodeC );
}

TEST( GraphTraversal,
      TheEdgeHookIsOptional )
{
    const auto      graph = chain();
    NodeOnlyVisitor visitor;
    grab::kernel::breadth_first_search( graph, nodeA, visitor );

    const std::vector<std::uint64_t> expected{ nodeA, nodeB, nodeC };
    EXPECT_EQ( visitor.nodes, expected );
}

TEST( GraphTraversal,
      DepthFirstFollowsTheFirstEdgeBeforeTheSecond )
{
    Graph graph;
    ASSERT_TRUE( graph.add_node( nodeA ) );
    ASSERT_TRUE( graph.add_node( nodeB ) );
    ASSERT_TRUE( graph.add_node( nodeC ) );
    ASSERT_TRUE( graph.add_node( nodeD ) );
    ASSERT_TRUE( graph.add_edge( nodeA, nodeB, firstPayload ) );
    ASSERT_TRUE( graph.add_edge( nodeA, nodeC, firstPayload ) );
    ASSERT_TRUE( graph.add_edge( nodeB, nodeD, firstPayload ) );

    RecordingVisitor visitor;
    grab::kernel::depth_first_search( graph, nodeA, visitor );

    const std::vector<std::uint64_t> expected{ nodeA, nodeB, nodeD, nodeC };
    EXPECT_EQ( visitor.nodes, expected );
}

TEST( GraphTraversal,
      TerminatesOnACycle )
{
    Graph graph;
    ASSERT_TRUE( graph.add_node( nodeA ) );
    ASSERT_TRUE( graph.add_node( nodeB ) );
    ASSERT_TRUE( graph.add_edge( nodeA, nodeB, firstPayload ) );
    ASSERT_TRUE( graph.add_edge( nodeB, nodeA, firstPayload ) );

    RecordingVisitor breadth;
    grab::kernel::breadth_first_search( graph, nodeA, breadth );
    EXPECT_EQ( breadth.nodes.size(), twoEntries );

    RecordingVisitor depth;
    grab::kernel::depth_first_search( graph, nodeA, depth );
    EXPECT_EQ( depth.nodes.size(), twoEntries );
}

TEST( GraphTraversal,
      AnAbsentStartNodeVisitsNothing )
{
    const auto       graph = chain();
    RecordingVisitor visitor;
    grab::kernel::breadth_first_search( graph, absentNode, visitor );
    grab::kernel::depth_first_search( graph, absentNode, visitor );

    EXPECT_TRUE( visitor.nodes.empty() );
    EXPECT_TRUE( visitor.edges.empty() );
}

TEST( GraphDelta,
      SeesAnAddedNode )
{
    Graph before;
    Graph after;
    ASSERT_TRUE( after.add_node( nodeA ) );

    const auto delta = grab::kernel::graph_difference( before, after );
    ASSERT_EQ( delta.added_nodes.size(), oneEntry );
    EXPECT_EQ( delta.added_nodes.front(), nodeA );
    EXPECT_TRUE( delta.removed_nodes.empty() );
}

TEST( GraphDelta,
      SeesARemovedNodeAndTheEdgesThatWentWithIt )
{
    auto       before = chain();
    Graph      after;

    const auto delta = grab::kernel::graph_difference( before, after );
    EXPECT_EQ( delta.removed_nodes.size(), threeEntries );
    EXPECT_EQ( delta.removed_edges.size(), twoEntries );
    EXPECT_TRUE( delta.added_nodes.empty() );
    EXPECT_TRUE( delta.added_edges.empty() );
}

TEST( GraphDelta,
      ReportsAnEdgeWhosePayloadChanged )
{
    Graph before;
    Graph after;
    for( Graph* graph : { &before, &after } )
    {
        ASSERT_TRUE( graph->add_node( nodeA ) );
        ASSERT_TRUE( graph->add_node( nodeB ) );
    }
    ASSERT_TRUE( before.add_edge( nodeA, nodeB, firstPayload ) );
    ASSERT_TRUE( after.add_edge( nodeA, nodeB, secondPayload ) );

    const auto delta = grab::kernel::graph_difference( before, after );
    ASSERT_EQ( delta.changed_edges.size(), oneEntry );
    EXPECT_EQ( delta.changed_edges.front().first, nodeA );
    EXPECT_EQ( delta.changed_edges.front().second, nodeB );
    EXPECT_TRUE( delta.added_edges.empty() );
    EXPECT_TRUE( delta.removed_edges.empty() );
}

TEST( GraphDelta,
      ReportsNothingForAnUnchangedGraph )
{
    const auto before = chain();
    const auto after  = chain();

    const auto delta  = grab::kernel::graph_difference( before, after );
    EXPECT_TRUE( delta.added_nodes.empty() );
    EXPECT_TRUE( delta.removed_nodes.empty() );
    EXPECT_TRUE( delta.added_edges.empty() );
    EXPECT_TRUE( delta.removed_edges.empty() );
    EXPECT_TRUE( delta.changed_edges.empty() );
}

TEST( GraphDelta,
      KeepsHashCollidingEndpointPairsDistinct )
{
    Graph before;
    Graph after;
    for( Graph* graph : { &before, &after } )
    {
        ASSERT_TRUE( graph->add_node( nodeA ) );
        ASSERT_TRUE( graph->add_node( nodeB ) );
        ASSERT_TRUE( graph->add_node( collidingFrom ) );
        ASSERT_TRUE( graph->add_node( collidingTo ) );
    }
    ASSERT_TRUE( before.add_edge( nodeA, nodeB, firstPayload ) );
    ASSERT_TRUE( after.add_edge( collidingFrom, collidingTo, firstPayload ) );

    const auto delta = grab::kernel::graph_difference( before, after );
    ASSERT_EQ( delta.added_edges.size(), oneEntry );
    ASSERT_EQ( delta.removed_edges.size(), oneEntry );
    EXPECT_EQ( delta.added_edges.front().first, collidingFrom );
    EXPECT_EQ( delta.added_edges.front().second, collidingTo );
    EXPECT_EQ( delta.removed_edges.front().first, nodeA );
    EXPECT_EQ( delta.removed_edges.front().second, nodeB );
}
