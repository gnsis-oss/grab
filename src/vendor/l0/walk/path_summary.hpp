#pragma once
// ┌──────────────────────────────────────────────────────────────────────┐
// │  walk/path_summary.h -- Naiad-Ψ precomputed path summaries           │
// └──────────────────────────────────────────────────────────────────────┘
//
// Precomputes per-source BFS layers over a graph so that queries
// (reachable? hop distance? next hop?) answer in O(1).
//
// Static use: construct once over a fixed graph, query repeatedly.
//
// Incremental use: when the underlying graph mutates, the consumer
// notifies the summary via on_edge_added / on_edge_removed; the summary
// rebuilds its internal state from the current graph state. The graph
// reference passed at construction must remain valid for the lifetime
// of the PathSummary; consumers mutate the graph then call the hook.
//
// PathInfo is hop-distance (treats the graph as unweighted). Consumers
// who want weighted summaries can compose with walk::path<FloydWarshall>.

#include <cstddef>
#include <queue>
#include <type_traits>
#include <unordered_map>
#include <web/concept.hpp>

namespace walk
{

    template<typename Knot>
    struct PathInfo
    {
            bool        reachable = false;
            std::size_t distance  = 0;
            Knot        next_hop  = Knot{};    // first hop from src toward dst
    };

    template<web::Graph G>
    class PathSummary
    {
        public:

            using Knot = G::knot_type;
            using Edge = G::edge_type;

            explicit PathSummary( const G& graph ) :
                graph_( graph )
            {
                rebuild();
            }

            [[nodiscard]]
            PathInfo<Knot>
            query( Knot src,
                   Knot dst ) const
            {
                PathInfo<Knot> info;
                auto           src_it = per_source_.find( src );
                if( src_it == per_source_.end() )
                {
                    return info;
                }
                const auto& layer   = src_it->second;
                auto        dist_it = layer.distance.find( dst );
                if( dist_it == layer.distance.end() )
                {
                    return info;
                }
                info.reachable = true;
                info.distance  = dist_it->second;
                if( src == dst )
                {
                    info.next_hop = src;
                }
                else
                {
                    info.next_hop = layer.next_hop.at( dst );
                }
                return info;
            }

            void
            on_edge_added( Knot /*from*/,
                           Knot /*to*/ )
            {
                rebuild();
            }

            void
            on_edge_removed( Knot /*from*/,
                             Knot /*to*/ )
            {
                rebuild();
            }

        private:

            struct Layer
            {
                    std::unordered_map<Knot, std::size_t> distance;
                    std::unordered_map<Knot, Knot>        next_hop;
            };

            // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
            const G&                        graph_;
            std::unordered_map<Knot, Layer> per_source_;

            void
            rebuild()
            {
                per_source_.clear();
                for( const auto& src : graph_.knots() )
                {
                    per_source_.emplace( src, bfs_from( src ) );
                }
            }

            [[nodiscard]]
            Layer
            bfs_from( Knot src ) const
            {
                Layer                          layer;
                std::unordered_map<Knot, Knot> parent;
                std::queue<Knot>               q;

                layer.distance[src] = 0;
                q.push( src );

                while( !q.empty() )
                {
                    const Knot u = q.front();
                    q.pop();
                    const std::size_t du = layer.distance.at( u );
                    for( const auto& adj : graph_.out( u ) )
                    {
                        Knot v;
                        if constexpr( std::is_void_v<Edge> )
                        {
                            v = adj;
                        }
                        else
                        {
                            v = adj.target;
                        }
                        if( layer.distance.find( v ) != layer.distance.end() )
                        {
                            continue;
                        }
                        layer.distance[v] = du + 1;
                        parent[v]         = u;
                        q.push( v );
                    }
                }

                // Compute next_hop[v] = first edge on path src -> v.
                // Walk parent chain back to src; the child of src is the next hop.
                for( const auto& [v, _] : layer.distance )
                {
                    if( v == src )
                    {
                        continue;
                    }
                    Knot cur = v;
                    while( true )
                    {
                        auto it = parent.find( cur );
                        if( it == parent.end() || it->second == src )
                        {
                            break;
                        }
                        cur = it->second;
                    }
                    layer.next_hop[v] = cur;
                }

                return layer;
            }
    };

}    // namespace walk
