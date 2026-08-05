#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

// What changed between two snapshots of the same graph.
//
// Both the node set and every adjacency list are held in ascending order, so
// this compares them by merging sorted ranges rather than by hashing. That is
// not only cheaper — it removes a whole class of bug. Folding an endpoint pair
// into a single hash makes distinct edges collide and silently drop out of the
// delta, and grab derives its widget events from exactly this comparison, so a
// dropped edge is a missing event rather than a slow path.

#include "kernel/graph/adjacency_graph.hpp"

#include <cstddef>
#include <functional>
#include <span>
#include <utility>
#include <vector>

namespace grab::kernel
{

    template<typename Key>
    struct GraphDelta
    {
            std::vector<Key>                 added_nodes;
            std::vector<Key>                 removed_nodes;
            std::vector<std::pair<Key, Key>> added_edges;
            std::vector<std::pair<Key, Key>> removed_edges;
            // Present on both sides, but carrying a different payload.
            std::vector<std::pair<Key, Key>> changed_edges;
    };

    namespace detail
    {

        // Merge one node's two adjacency lists, both ascending by target.
        template<typename Key,
                 typename Edge,
                 typename PayloadEqual>
        void
        merge_edge_lists( Key                   source,
                          std::span<const Edge> before,
                          std::span<const Edge> after,
                          const PayloadEqual&   payload_equal,
                          GraphDelta<Key>&      delta )
        {
            std::size_t left  = 0U;
            std::size_t right = 0U;

            while( left < before.size() && right < after.size() )
            {
                if( before[left].target < after[right].target )
                {
                    delta.removed_edges.emplace_back( source, before[left].target );
                    ++left;
                }
                else if( after[right].target < before[left].target )
                {
                    delta.added_edges.emplace_back( source, after[right].target );
                    ++right;
                }
                else
                {
                    if( !payload_equal( before[left].payload, after[right].payload ) )
                    {
                        delta.changed_edges.emplace_back( source, after[right].target );
                    }
                    ++left;
                    ++right;
                }
            }
            for( ; left < before.size(); ++left )
            {
                delta.removed_edges.emplace_back( source, before[left].target );
            }
            for( ; right < after.size(); ++right )
            {
                delta.added_edges.emplace_back( source, after[right].target );
            }
        }

    }    // namespace detail

    // Node sets merge in one pass; each node's adjacency lists merge in one
    // more, reached by a map lookup. Edges are read through spans, so the only
    // allocation is the delta itself.
    template<typename Key,
             typename Payload,
             typename PayloadEqual = std::equal_to<>>
    [[nodiscard]]
    GraphDelta<Key>
    graph_difference( const AdjacencyGraph<Key,
                                           Payload>& before,
                      const AdjacencyGraph<Key,
                                           Payload>& after,
                      const PayloadEqual&            payload_equal = PayloadEqual{} )
    {
        using Edge = typename AdjacencyGraph<Key, Payload>::edge_type;

        GraphDelta<Key> delta;

        auto            before_nodes = before.nodes();
        auto            after_nodes  = after.nodes();
        auto            left         = before_nodes.begin();
        auto            right        = after_nodes.begin();

        const auto      left_end     = before_nodes.end();
        const auto      right_end    = after_nodes.end();

        const auto      only_before  = [&]( Key node )
        {
            delta.removed_nodes.push_back( node );
            detail::merge_edge_lists<Key, Edge>( node,
                                                 before.out_edges( node ),
                                                 {},
                                                 payload_equal,
                                                 delta );
        };
        const auto only_after = [&]( Key node )
        {
            delta.added_nodes.push_back( node );
            detail::merge_edge_lists<Key, Edge>( node,
                                                 {},
                                                 after.out_edges( node ),
                                                 payload_equal,
                                                 delta );
        };

        while( left != left_end && right != right_end )
        {
            if( *left < *right )
            {
                only_before( *left );
                ++left;
            }
            else if( *right < *left )
            {
                only_after( *right );
                ++right;
            }
            else
            {
                detail::merge_edge_lists<Key, Edge>( *left,
                                                     before.out_edges( *left ),
                                                     after.out_edges( *right ),
                                                     payload_equal,
                                                     delta );
                ++left;
                ++right;
            }
        }
        for( ; left != left_end; ++left )
        {
            only_before( *left );
        }
        for( ; right != right_end; ++right )
        {
            only_after( *right );
        }

        return delta;
    }

}    // namespace grab::kernel
