#pragma once
// ┌──────────────────────────────────────────────────────────────────────┐
// │  walk/backwards_spf.h -- destination-rooted shortest-path-first      │
// └──────────────────────────────────────────────────────────────────────┘
//
// Given a destination knot, computes shortest distances and next-hop
// pointers from every reachable knot back to the destination, by running
// Dijkstra over reversed edges (uses graph.in() instead of graph.out()).
//
// The result is a "sink tree" rooted at dest: every entry in next_hop
// points one edge closer to dest along a shortest path. Federation /
// mesh routing consumers can install (dst, next_hop) entries directly.
//
// Visitor hooks (optional, detected via if constexpr + requires):
//   on_find(knot)        -- knot first discovered
//   on_relax(from, to)   -- reverse edge relaxed (from is upstream)
//   on_settle(knot)      -- knot finalized

#include <heap/heap.hpp>
#include <log/writer.hpp>
#include <tag/tag.hpp>
#include <unordered_map>
#include <walk/path.hpp>
#include <web/concept.hpp>
#include <web/trait.hpp>

namespace walk
{

    template<typename Knot, typename Edge>
    struct BackwardsTree
    {
            Knot                                           dest;
            std::unordered_map<Knot, detail::Weight<Edge>> distances;
            std::unordered_map<Knot, Knot>                 next_hop;
    };

    namespace detail
    {

        template<web::Graph G,
                 typename Vis>
        requires web::Weighted<typename G::edge_type>
        [[nodiscard]]
        BackwardsTree<typename G::knot_type,
                      typename G::edge_type>
        backwards_spf_impl( const G&              graph,
                            typename G::knot_type dest,
                            Vis&                  visitor )
        {
            using Knot  = G::knot_type;
            using Edge  = G::edge_type;
            using Entry = PathEntry<Knot, Edge>;
            using W     = Weight<Edge>;

            BackwardsTree<Knot, Edge> tree;
            tree.dest = dest;

            if( !graph.has( dest ) )
            {
                logger::error( logger::tag( "walk.algo" ),
                               "backwards_spf() dest not in graph" );
                return tree;
            }

            heap::Heap<Entry, heap::Min> pq;
            tree.distances[dest] = W{};
            [[maybe_unused]]
            auto init = pq.add( Entry{ dest.id(), dest, W{} } );

            if constexpr( requires { visitor.on_find( dest ); } )
            {
                visitor.on_find( dest );
            }

            while( !pq.is_empty() )
            {
                auto        pop_result = pq.pop();
                const Entry entry      = *pop_result.ok();

                if constexpr( requires { visitor.on_settle( entry.knot ); } )
                {
                    visitor.on_settle( entry.knot );
                }

                // Walk reverse edges: from u, look at upstream neighbors via in().
                for( const auto& adj : graph.in( entry.knot ) )
                {
                    const Knot upstream = adj.target;
                    const W    edge_w   = web::Trait<Edge>::weight( adj.data );
                    const W    new_dist = entry.distance + edge_w;

                    auto       it       = tree.distances.find( upstream );
                    if( it == tree.distances.end() || new_dist < it->second )
                    {
                        tree.distances[upstream] = new_dist;
                        tree.next_hop[upstream]  = entry.knot;

                        if constexpr( requires {
                                          visitor.on_relax( upstream, entry.knot );
                                      } )
                        {
                            visitor.on_relax( upstream, entry.knot );
                        }

                        if( pq.has( upstream.id() ) )
                        {
                            [[maybe_unused]]
                            auto r =
                                pq.rank( Entry{ upstream.id(), upstream, new_dist } );
                        }
                        else
                        {
                            if constexpr( requires { visitor.on_find( upstream ); } )
                            {
                                visitor.on_find( upstream );
                            }
                            [[maybe_unused]]
                            auto r =
                                pq.add( Entry{ upstream.id(), upstream, new_dist } );
                        }
                    }
                }
            }

            return tree;
        }

        struct BackwardsSpfNoVisitor
        {
        };

    }    // namespace detail

    template<web::Graph G>
    requires web::Weighted<typename G::edge_type>
    [[nodiscard]]
    BackwardsTree<typename G::knot_type,
                  typename G::edge_type>
    backwards_spf( const G&              graph,
                   typename G::knot_type dest )
    {
        detail::BackwardsSpfNoVisitor nv;
        return detail::backwards_spf_impl( graph, dest, nv );
    }

    template<web::Graph G,
             typename Vis>
    requires web::Weighted<typename G::edge_type>
    [[nodiscard]]
    BackwardsTree<typename G::knot_type,
                  typename G::edge_type>
    backwards_spf( const G&              graph,
                   typename G::knot_type dest,
                   Vis&                  visitor )
    {
        return detail::backwards_spf_impl( graph, dest, visitor );
    }

}    // namespace walk
