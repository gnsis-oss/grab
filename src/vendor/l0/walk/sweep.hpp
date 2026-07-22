#pragma once
// ┌──────────────────────────────────────────────────────────────────────┐
// │  walk/sweep.h -- walk::sweep BFS traversal                          │
// └──────────────────────────────────────────────────────────────────────┘
//
// Visits knots in breadth-first order starting from start.
// Uses std::queue<Knot> internally. Each knot visited exactly once.
// If start is not in the graph, no knots are visited (no-op).
//
// Accepts any type satisfying web::Graph<G>. When the graph also satisfies
// web::DenseGraph<G>, uses flat vector<bool> for visited instead of
// unordered_set (significantly faster for large graphs).
//
// Visitor hooks (all optional, detected via if constexpr + requires):
//   on(knot)             -- existing: knot visited (required by Visitor concept)
//   on_find(knot)        -- knot first discovered (added to queue)
//   on_enter(knot)       -- knot dequeued and about to be processed
//   on_edge(from, to)    -- edge traversed during exploration
//   on_leave(knot)       -- knot fully processed (all neighbors examined)
//   on_skip(from, to)    -- edge to already-visited knot

#include <log/writer.hpp>
#include <out/detail/platform.hpp>
#include <queue>
#include <type_traits>
#include <unordered_set>
#include <vector>
#include <walk/visitor.hpp>
#include <web/concept.hpp>

namespace walk
{

    // ── sweep (BFS) ──────────────────────────────────────────────────

    template<web::Graph G,
             typename V>
    requires VisitorOf<V,
                       typename G::knot_type>
    void
    sweep( const G&              graph,
           typename G::knot_type start,
           V&                    visitor )
    {
        using Knot = G::knot_type;
        using Edge = G::edge_type;

        logger::trace( logger::tag( "walk.algo" ),
                       "sweep() BFS starting knots={}",
                       graph.knots().size() );
        if( !graph.has( start ) )
        {
            logger::error( logger::tag( "walk.algo" ),
                           "sweep() start knot not in graph" );
            return;
        }

        // ── Visited bookkeeping: dense or sparse ─────────────────────

        auto run = [&]( auto is_visited, auto mark_visited )
        {
            mark_visited( start );
            std::queue<Knot> queue;
            queue.push( start );

            if constexpr( requires { visitor.on_find( start ); } )
            {
                visitor.on_find( start );
            }

            while( !queue.empty() )
            {
                const Knot knot = queue.front();
                queue.pop();

                if constexpr( requires { visitor.on_enter( knot ); } )
                {
                    visitor.on_enter( knot );
                }

                visitor.on( knot );

                {
                    const auto& adj_list = graph.out( knot );
                    for( auto it_adj = adj_list.begin(); it_adj != adj_list.end();
                         ++it_adj )
                    {
                        const auto& entry = *it_adj;
                        // Prefetch next neighbor's adjacency data
                        auto        next_adj = std::next( it_adj );
                        if( next_adj != adj_list.end() )
                        {
                            SEED_PREFETCH( &( *next_adj ) );
                        }

                        Knot neighbor;
                        if constexpr( std::is_void_v<Edge> )
                        {
                            neighbor = entry;
                        }
                        else
                        {
                            neighbor = entry.target;
                        }

                        if constexpr( requires { visitor.on_edge( knot, neighbor ); } )
                        {
                            visitor.on_edge( knot, neighbor );
                        }

                        if( !is_visited( neighbor ) )
                        {
                            mark_visited( neighbor );

                            if constexpr( requires { visitor.on_find( neighbor ); } )
                            {
                                visitor.on_find( neighbor );
                            }
                            queue.push( neighbor );
                        }
                        else
                        {
                            if constexpr( requires {
                                              visitor.on_skip( knot, neighbor );
                                          } )
                            {
                                visitor.on_skip( knot, neighbor );
                            }
                        }
                    }
                }

                if constexpr( requires { visitor.on_leave( knot ); } )
                {
                    visitor.on_leave( knot );
                }
            }
        };

        // ── Dispatch: DenseGraph uses flat vector, otherwise hash set ──

        if constexpr( web::DenseGraph<G> )
        {
            std::vector<bool> visited( graph.dense_size(), false );

            run(
                [&]( Knot k )
                {
                    return visited[graph.dense_id( k )];
                },
                [&]( Knot k )
                {
                    visited[graph.dense_id( k )] = true;
                }
            );
        }
        else
        {
            std::unordered_set<Knot> visited;

            run(
                [&]( Knot k )
                {
                    return visited.contains( k );
                },
                [&]( Knot k )
                {
                    visited.insert( k );
                }
            );
        }
    }

}    // namespace walk
