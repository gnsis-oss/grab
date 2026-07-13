#pragma once
// ┌──────────────────────────────────────────────────────────────────────┐
// │  walk/delve.h -- walk::delve DFS traversal                          │
// └──────────────────────────────────────────────────────────────────────┘
//
// Visits knots in depth-first order starting from start.
// Uses std::vector<Knot> as an iterative stack (avoids stack overflow).
// Each knot visited exactly once.
// If start is not in the graph, no-op.
//
// Accepts any type satisfying web::Graph<G>. When the graph also satisfies
// web::DenseGraph<G>, uses flat vector<bool> for visited/on_stack instead of
// unordered_set (significantly faster for large graphs).
//
// Visitor hooks (all optional, detected via if constexpr + requires):
//   on(knot)                -- existing: knot visited (required by Visitor concept)
//   on_find(knot)           -- knot first discovered (pushed to stack)
//   on_enter(knot)          -- knot popped and about to be processed
//   on_edge(from, to)       -- edge traversed during exploration
//   on_tie(from, neighbor)  -- edge with data (weighted graphs only, Neighbor<Edge>)
//   on_leave(knot)          -- knot fully processed (all neighbors examined)
//   on_skip(from, to)       -- edge to already-visited knot
//   on_back_edge(from, to)  -- back edge detected (cycle)

#include <log/writer.hpp>
#include <type_traits>
#include <unordered_set>
#include <vector>
#include <walk/visitor.hpp>
#include <web/concept.hpp>

namespace walk
{

    template<web::Graph G,
             typename V>
    requires VisitorOf<V,
                       typename G::knot_type>
    void
    delve( const G&              graph,
           typename G::knot_type start,
           V&                    visitor )
    {
        using Knot = G::knot_type;
        using Edge = G::edge_type;

        logger::trace( logger::tag( "walk.algo" ),
                       "delve() DFS starting knots={}",
                       graph.knots().size() );
        if( !graph.has( start ) )
        {
            logger::error( logger::tag( "walk.algo" ),
                           "delve() start knot not in graph" );
            return;
        }

        // Stack holds (knot, parent) pairs for back-edge detection
        struct Frame
        {
                Knot knot;
                Knot parent;
        };

        // ── Core logic: parameterised over bookkeeping lambdas ─────────

        auto run = [&]( auto is_visited,
                        auto mark_visited,
                        auto is_on_stack,    // NOLINT(misc-unused-parameters)
                        auto mark_on_stack )
        {
            std::vector<Frame> stack;

            stack.push_back( Frame{ start, Knot{} } );

            if constexpr( requires { visitor.on_find( start ); } )
            {
                visitor.on_find( start );
            }

            while( !stack.empty() )
            {
                const Frame frame = stack.back();
                stack.pop_back();

                if( is_visited( frame.knot ) )
                {
                    continue;
                }
                mark_visited( frame.knot );
                mark_on_stack( frame.knot );

                if constexpr( requires { visitor.on_enter( frame.knot ); } )
                {
                    visitor.on_enter( frame.knot );
                }

                visitor.on( frame.knot );

                // Push neighbors in reverse order so that the first neighbor
                // is processed first (natural DFS order: depth before breadth)
                auto neighbors = graph.out( frame.knot );
                for( auto it = neighbors.rbegin(); it != neighbors.rend(); ++it )
                {
                    Knot neighbor;
                    if constexpr( std::is_void_v<Edge> )
                    {
                        neighbor = *it;
                    }
                    else
                    {
                        neighbor = it->target;
                    }

                    if constexpr( requires { visitor.on_edge( frame.knot, neighbor ); } )
                    {
                        visitor.on_edge( frame.knot, neighbor );
                    }

                    // When Edge is not void, pass full Neighbor<Edge> if visitor has
                    // on_tie hook
                    if constexpr( !std::is_void_v<Edge> )
                    {
                        if constexpr( requires { visitor.on_tie( frame.knot, *it ); } )
                        {
                            visitor.on_tie( frame.knot, *it );
                        }
                    }

                    if( !is_visited( neighbor ) )
                    {
                        if constexpr( requires { visitor.on_find( neighbor ); } )
                        {
                            visitor.on_find( neighbor );
                        }
                        stack.push_back( Frame{ neighbor, frame.knot } );
                    }
                    else
                    {
                        if constexpr( requires {
                                          visitor.on_skip( frame.knot, neighbor );
                                      } )
                        {
                            visitor.on_skip( frame.knot, neighbor );
                        }
                        if constexpr( requires {
                                          visitor.on_back_edge( frame.knot, neighbor );
                                      } )
                        {
                            if( is_on_stack( neighbor ) )
                            {
                                visitor.on_back_edge( frame.knot, neighbor );
                            }
                        }
                    }
                }

                if constexpr( requires { visitor.on_leave( frame.knot ); } )
                {
                    visitor.on_leave( frame.knot );
                }
            }
        };

        // ── Dispatch: DenseGraph uses flat vectors, otherwise hash sets ──

        if constexpr( web::DenseGraph<G> )
        {
            std::vector<bool> visited( graph.dense_size(), false );
            std::vector<bool> on_stack( graph.dense_size(), false );

            run(
                [&]( Knot k )
                {
                    return visited[graph.dense_id( k )];
                },
                [&]( Knot k )
                {
                    visited[graph.dense_id( k )] = true;
                },
                [&]( Knot k )
                {
                    return on_stack[graph.dense_id( k )];
                },
                [&]( Knot k )
                {
                    on_stack[graph.dense_id( k )] = true;
                }
            );
        }
        else
        {
            std::unordered_set<Knot> visited;
            std::unordered_set<Knot> on_stack;

            run(
                [&]( Knot k )
                {
                    return visited.contains( k );
                },
                [&]( Knot k )
                {
                    visited.insert( k );
                },
                [&]( Knot k )
                {
                    return on_stack.contains( k );
                },
                [&]( Knot k )
                {
                    on_stack.insert( k );
                }
            );
        }
    }

}    // namespace walk
