#pragma once
// ┌──────────────────────────────────────────────────────────────────────┐
// │  walk/knot.h -- strongly connected components (SCC)                 │
// └──────────────────────────────────────────────────────────────────────┘
//
// Strategies:
//   Tarjan (default) -- Tarjan's SCC algorithm (DFS + low-link)
//   Kosaraju         -- Kosaraju's two-pass DFS
//
// Returns vector of components, each component a vector of knots.
// Only works on directed graphs.
//
// Accepts any type satisfying web::Graph<G>.
//
// Visitor hooks (optional):
//   on_find(knot)            -- knot first discovered
//   on_knot_found(span)      -- SCC component found (span of knots)

#include <algorithm>
#include <cstddef>
#include <log/writer.hpp>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <walk/strategy.hpp>
#include <web/concept.hpp>

namespace walk
{

    struct KnotNoVisitor
    {
    };

    namespace detail
    {

        // ── Tarjan's SCC ────────────────────────────────────────────────────

        template<web::Graph G,
                 typename Vis>
        [[nodiscard]]
        auto
        knot_tarjan( const G& graph,
                     Vis&     visitor )
        {
            using Knot     = G::knot_type;
            using Edge     = G::edge_type;

            auto all_knots = graph.knots();
            if( all_knots.empty() )
            {
                return std::vector<std::vector<Knot>>{};
            }

            std::vector<std::vector<Knot>> components;

            std::unordered_map<Knot, int>  index_map;
            std::unordered_map<Knot, int>  lowlink;
            std::unordered_set<Knot>       on_stack;
            std::vector<Knot>              stack;
            int                            index_counter = 0;

            // Iterative Tarjan to avoid stack overflow
            struct Frame
            {
                    Knot        knot;
                    std::size_t adj_index;
                    bool        returned_from_child;
                    Knot        child;    // the child we returned from
            };

            for( const auto& start : all_knots )
            {
                if( index_map.contains( start ) )
                {
                    continue;
                }

                std::vector<Frame> dfs_stack;
                dfs_stack.push_back( Frame{ start, 0, false, Knot{} } );

                // Initialize start
                index_map[start] = index_counter;
                lowlink[start]   = index_counter;
                index_counter++;
                stack.push_back( start );
                on_stack.insert( start );

                if constexpr( requires { visitor.on_find( start ); } )
                {
                    visitor.on_find( start );
                }

                while( !dfs_stack.empty() )
                {
                    auto& frame    = dfs_stack.back();
                    auto  out_span = graph.out( frame.knot );

                    // If returning from a child, update lowlink
                    if( frame.returned_from_child )
                    {
                        frame.returned_from_child = false;
                        auto it                   = lowlink.find( frame.child );
                        if( it != lowlink.end() && it->second < lowlink[frame.knot] )
                        {
                            lowlink[frame.knot] = it->second;
                        }
                    }

                    bool pushed_child = false;
                    while( frame.adj_index <
                           static_cast<std::size_t>( out_span.size() ) )
                    {
                        Knot neighbor;
                        if constexpr( std::is_void_v<Edge> )
                        {
                            neighbor = out_span[frame.adj_index];
                        }
                        else
                        {
                            neighbor = out_span[frame.adj_index].target;
                        }
                        frame.adj_index++;

                        if( !index_map.contains( neighbor ) )
                        {
                            // Not visited: push onto DFS stack
                            index_map[neighbor] = index_counter;
                            lowlink[neighbor]   = index_counter;
                            index_counter++;
                            stack.push_back( neighbor );
                            on_stack.insert( neighbor );

                            if constexpr( requires { visitor.on_find( neighbor ); } )
                            {
                                visitor.on_find( neighbor );
                            }

                            dfs_stack.push_back( Frame{ neighbor, 0, false, Knot{} } );
                            pushed_child = true;
                            // Mark parent that we'll return from this child
                            auto& parent               = dfs_stack[dfs_stack.size() - 2];
                            parent.returned_from_child = true;
                            parent.child               = neighbor;
                            break;
                        }
                        if( on_stack.contains( neighbor ) )
                        {
                            // On stack: update lowlink
                            if( index_map[neighbor] < lowlink[frame.knot] )
                            {
                                lowlink[frame.knot] = index_map[neighbor];
                            }
                        }
                    }

                    if( pushed_child )
                    {
                        continue;
                    }

                    // All neighbors processed
                    if( lowlink[frame.knot] == index_map[frame.knot] )
                    {
                        // Found an SCC root
                        std::vector<Knot> component;
                        while( true )
                        {
                            const Knot w = stack.back();
                            stack.pop_back();
                            on_stack.erase( w );
                            component.push_back( w );
                            if( w == frame.knot )
                            {
                                break;
                            }
                        }

                        if constexpr( requires {
                                          visitor.on_knot_found(
                                              std::span<Knot>( component )
                                          );
                                      } )
                        {
                            visitor.on_knot_found( std::span<Knot>( component ) );
                        }

                        components.push_back( std::move( component ) );
                    }

                    dfs_stack.pop_back();
                }
            }

            return components;
        }

        // ── Kosaraju's SCC ──────────────────────────────────────────────────

        template<web::Graph G,
                 typename Vis>
        [[nodiscard]]
        auto
        knot_kosaraju( const G& graph,
                       Vis&     visitor )
        {
            using Knot     = G::knot_type;
            using Edge     = G::edge_type;

            auto all_knots = graph.knots();
            if( all_knots.empty() )
            {
                return std::vector<std::vector<Knot>>{};
            }

            // Pass 1: DFS on original graph, compute finish order
            std::unordered_set<Knot> visited;
            std::vector<Knot>        finish_order;
            finish_order.reserve( all_knots.size() );

            struct Frame
            {
                    Knot        knot;
                    std::size_t adj_index;
            };

            for( const auto& start : all_knots )
            {
                if( visited.contains( start ) )
                {
                    continue;
                }

                std::vector<Frame> dfs_stack;
                dfs_stack.push_back( Frame{ start, 0 } );
                visited.insert( start );

                if constexpr( requires { visitor.on_find( start ); } )
                {
                    visitor.on_find( start );
                }

                while( !dfs_stack.empty() )
                {
                    auto& frame    = dfs_stack.back();
                    auto  out_span = graph.out( frame.knot );

                    if( frame.adj_index < static_cast<std::size_t>( out_span.size() ) )
                    {
                        Knot neighbor;
                        if constexpr( std::is_void_v<Edge> )
                        {
                            neighbor = out_span[frame.adj_index];
                        }
                        else
                        {
                            neighbor = out_span[frame.adj_index].target;
                        }
                        frame.adj_index++;

                        if( !visited.contains( neighbor ) )
                        {
                            visited.insert( neighbor );
                            if constexpr( requires { visitor.on_find( neighbor ); } )
                            {
                                visitor.on_find( neighbor );
                            }
                            dfs_stack.push_back( Frame{ neighbor, 0 } );
                        }
                    }
                    else
                    {
                        finish_order.push_back( frame.knot );
                        dfs_stack.pop_back();
                    }
                }
            }

            // Pass 2: DFS on transposed graph in reverse finish order
            visited.clear();
            std::vector<std::vector<Knot>> components;

            for( auto it = finish_order.rbegin(); it != finish_order.rend(); ++it )
            {
                const Knot start = *it;
                if( visited.contains( start ) )
                {
                    continue;
                }

                std::vector<Knot>  component;
                std::vector<Frame> dfs_stack;
                dfs_stack.push_back( Frame{ start, 0 } );
                visited.insert( start );

                while( !dfs_stack.empty() )
                {
                    auto& frame = dfs_stack.back();
                    // Use incoming edges (transpose)
                    auto  in_span = graph.in( frame.knot );

                    if( frame.adj_index < static_cast<std::size_t>( in_span.size() ) )
                    {
                        Knot neighbor;
                        if constexpr( std::is_void_v<Edge> )
                        {
                            neighbor = in_span[frame.adj_index];
                        }
                        else
                        {
                            neighbor = in_span[frame.adj_index].target;
                        }
                        frame.adj_index++;

                        if( !visited.contains( neighbor ) )
                        {
                            visited.insert( neighbor );
                            dfs_stack.push_back( Frame{ neighbor, 0 } );
                        }
                    }
                    else
                    {
                        component.push_back( frame.knot );
                        dfs_stack.pop_back();
                    }
                }

                if constexpr( requires {
                                  visitor.on_knot_found( std::span<Knot>( component ) );
                              } )
                {
                    visitor.on_knot_found( std::span<Knot>( component ) );
                }

                components.push_back( std::move( component ) );
            }

            return components;
        }

    }    // namespace detail

    // ── Public API ──────────────────────────────────────────────────────

    // Without visitor
    template<typename Strategy = Tarjan,
             web::Graph G>
    [[nodiscard]]
    auto
    knot( const G& graph )
    {
        logger::trace( logger::tag( "walk.algo" ),
                       "knot() SCC starting knots={}",
                       graph.knots().size() );
        KnotNoVisitor nv;
        if constexpr( std::is_same_v<Strategy, Tarjan> )
        {
            return detail::knot_tarjan( graph, nv );
        }
        else if constexpr( std::is_same_v<Strategy, Kosaraju> )
        {
            return detail::knot_kosaraju( graph, nv );
        }
        else
        {
            static_assert( std::is_same_v<Strategy, Tarjan>, "Unknown knot strategy" );
        }
    }

    // With visitor
    template<typename Strategy = Tarjan,
             web::Graph G,
             typename Vis>
    [[nodiscard]]
    auto
    knot( const G& graph,
          Vis&     visitor )
    {
        if constexpr( std::is_same_v<Strategy, Tarjan> )
        {
            return detail::knot_tarjan( graph, visitor );
        }
        else if constexpr( std::is_same_v<Strategy, Kosaraju> )
        {
            return detail::knot_kosaraju( graph, visitor );
        }
        else
        {
            static_assert( std::is_same_v<Strategy, Tarjan>, "Unknown knot strategy" );
        }
    }

}    // namespace walk
