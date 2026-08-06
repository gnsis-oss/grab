#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

// Kahn's algorithm over an AdjacencyGraph.
//
// Returns nullopt on a cycle, which is the only failure: a cycle is not a
// policy violation, it is a graph that cannot be linearised, and a runner
// waiting on a dependency that never resolves would simply never terminate.
//
// It CANNOT see a self-edge, and callers must not expect it to.
// AdjacencyGraph::add_edge rejects source == target and returns false, so the
// edge never enters the graph at all and the sort succeeds on the graph minus
// the rejected edge. Rejecting a self-dependency is the loader's job, at the
// point where add_edge returns false.
//
// Determinism: nodes() walks a std::map, so the seed set and every newly-ready
// node are appended in ascending key order. The same graph always yields the
// same linearization.
//
// Cost note: in_edges() is O(log n) rather than O(1) — the adjacency lists live
// in a std::map keyed by node.

#include "kernel/graph/adjacency_graph.hpp"

#include <cstddef>
#include <optional>
#include <unordered_map>
#include <vector>

namespace grab::kernel
{

    template<typename Key,
             typename Payload>
    [[nodiscard]]
    std::optional<std::vector<Key>>
    topological_order( const AdjacencyGraph<Key,
                                            Payload>& graph )
    {
        std::unordered_map<Key, std::size_t> remaining;
        remaining.reserve( graph.node_count() );
        for( const auto& node : graph.nodes() )
        {
            remaining[node] = graph.in_edges( node ).size();
        }

        std::vector<Key> order;
        order.reserve( graph.node_count() );
        for( const auto& node : graph.nodes() )
        {
            if( remaining[node] == 0U )
            {
                order.push_back( node );
            }
        }

        for( std::size_t cursor = 0U; cursor < order.size(); ++cursor )
        {
            const Key node = order[cursor];
            for( const auto& edge : graph.out_edges( node ) )
            {
                const auto found = remaining.find( edge.target );
                if( found == remaining.end() || found->second == 0U )
                {
                    continue;
                }
                --found->second;
                if( found->second == 0U )
                {
                    order.push_back( edge.target );
                }
            }
        }

        if( order.size() != graph.node_count() )
        {
            return std::nullopt;
        }
        return order;
    }

}    // namespace grab::kernel
