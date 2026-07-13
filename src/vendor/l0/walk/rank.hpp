#pragma once
// ┌──────────────────────────────────────────────────────────────────────┐
// │  walk/rank.h -- walk::rank topological sort                         │
// └──────────────────────────────────────────────────────────────────────┘
//
// Strategies:
//   Kahn (default)  -- BFS-based, Kahn's algorithm
//   DfsOrder        -- DFS-based topological sort
//
// Returns knots in topological order (every knot before all knots it
// has outgoing edges to), or out::Error::stuck if a cycle is detected.
//
// Accepts any type satisfying web::Graph<G>. When the graph also satisfies
// web::DenseGraph<G>, uses flat vectors for bookkeeping instead of
// unordered containers (significantly faster for large graphs).
//
// NOTE: topological sort only makes sense on directed graphs. If called on
// an undirected graph the result is meaningless -- this is the caller's
// responsibility to ensure.
//
// Visitor hooks (optional):
//   on_rank(knot, rank_number) -- knot gets its rank number (0-based position)

#include <algorithm>
#include <cstdint>
#include <log/writer.hpp>
#include <out/put.hpp>
#include <queue>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include <walk/strategy.hpp>
#include <web/concept.hpp>

namespace walk
{

    // ── Default no-visitor ──────────────────────────────────────────────

    struct RankNoVisitor
    {
    };

    // ── Kahn's algorithm (default) ──────────────────────────────────────

    namespace detail
    {

        template<web::Graph G,
                 typename Vis>
        [[nodiscard]]
        out::Put<std::vector<typename G::knot_type>,
                 out::Error>
        rank_kahn( const G& graph,
                   Vis&     visitor )
        {
            using Knot = G::knot_type;
            using Edge = G::edge_type;

            auto knots = graph.knots();
            if( knots.empty() )
            {
                return std::vector<Knot>{};
            }

            // ── Core algorithm parameterised on bookkeeping accessors ──

            auto run = [&]( auto get_degree,
                            auto set_degree,
                            auto inc_degree,
                            auto dec_degree ) -> out::Put<std::vector<Knot>, out::Error>
            {
                // Initialise in-degrees to zero
                for( const auto& k : knots )
                {
                    set_degree( k, 0 );
                }

                // Compute in-degrees
                for( const auto& k : knots )
                {
                    for( const auto& entry : graph.out( k ) )
                    {
                        Knot neighbor;
                        if constexpr( std::is_void_v<Edge> )
                        {
                            neighbor = entry;
                        }
                        else
                        {
                            neighbor = entry.target;
                        }
                        inc_degree( neighbor );
                    }
                }

                // Enqueue all knots with in-degree 0
                std::queue<Knot> queue;
                for( const auto& k : knots )
                {
                    if( get_degree( k ) == 0 )
                    {
                        queue.push( k );
                    }
                }

                std::vector<Knot> result;
                result.reserve( knots.size() );

                while( !queue.empty() )
                {
                    const Knot knot = queue.front();
                    queue.pop();

                    if constexpr( requires { visitor.on_rank( knot, result.size() ); } )
                    {
                        visitor.on_rank( knot, result.size() );
                    }

                    result.push_back( knot );

                    for( const auto& entry : graph.out( knot ) )
                    {
                        Knot neighbor;
                        if constexpr( std::is_void_v<Edge> )
                        {
                            neighbor = entry;
                        }
                        else
                        {
                            neighbor = entry.target;
                        }
                        dec_degree( neighbor );
                        if( get_degree( neighbor ) == 0 )
                        {
                            queue.push( neighbor );
                        }
                    }
                }

                if( result.size() != knots.size() )
                {
                    return out::Error::stuck;
                }
                return result;
            };

            // ── Dispatch: DenseGraph uses flat vector, otherwise hash map ──

            if constexpr( web::DenseGraph<G> )
            {
                std::vector<std::size_t> in_degree( graph.dense_size(), 0 );

                return run(
                    [&]( Knot k ) -> std::size_t
                    {
                        return in_degree[graph.dense_id( k )];
                    },
                    [&]( Knot k, std::size_t v )
                    {
                        in_degree[graph.dense_id( k )] = v;
                    },
                    [&]( Knot k )
                    {
                        ++in_degree[graph.dense_id( k )];
                    },
                    [&]( Knot k )
                    {
                        --in_degree[graph.dense_id( k )];
                    }
                );
            }
            else
            {
                std::unordered_map<Knot, std::size_t> in_degree;

                return run(
                    [&]( Knot k ) -> std::size_t
                    {
                        return in_degree[k];
                    },
                    [&]( Knot k, std::size_t v )
                    {
                        in_degree[k] = v;
                    },
                    [&]( Knot k )
                    {
                        ++in_degree[k];
                    },
                    [&]( Knot k )
                    {
                        --in_degree[k];
                    }
                );
            }
        }

        // ── DFS-based topological sort ──────────────────────────────────────

        template<web::Graph G,
                 typename Vis>
        [[nodiscard]]
        out::Put<std::vector<typename G::knot_type>,
                 out::Error>
        rank_dfs( const G& graph,
                  Vis&     visitor )
        {
            using Knot = G::knot_type;
            using Edge = G::edge_type;

            auto knots = graph.knots();
            if( knots.empty() )
            {
                return std::vector<Knot>{};
            }

            enum class State : std::uint8_t
            {
                White,
                Gray,
                Black,
            };

            // ── Core algorithm parameterised on color bookkeeping ──

            auto run = [&]( auto get_color,
                            auto set_color ) -> out::Put<std::vector<Knot>, out::Error>
            {
                for( const auto& k : knots )
                {
                    set_color( k, State::White );
                }

                std::vector<Knot> result;
                result.reserve( knots.size() );
                bool has_cycle = false;

                // Iterative DFS with state tracking
                struct Frame
                {
                        Knot        knot;
                        std::size_t adj_index;    // current neighbor index
                };

                for( const auto& start : knots )
                {
                    if( get_color( start ) != State::White )
                    {
                        continue;
                    }

                    std::vector<Frame> stack;
                    stack.push_back( Frame{ start, 0 } );
                    set_color( start, State::Gray );

                    while( !stack.empty() && !has_cycle )
                    {
                        auto& frame    = stack.back();
                        auto  out_span = graph.out( frame.knot );

                        if( frame.adj_index <
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

                            auto c = get_color( neighbor );
                            if( c == State::White )
                            {
                                set_color( neighbor, State::Gray );
                                stack.push_back( Frame{ neighbor, 0 } );
                            }
                            else if( c == State::Gray )
                            {
                                has_cycle = true;
                            }
                        }
                        else
                        {
                            // All neighbors processed
                            set_color( frame.knot, State::Black );
                            result.push_back( frame.knot );
                            stack.pop_back();
                        }
                    }

                    if( has_cycle )
                    {
                        return out::Error::stuck;
                    }
                }

                // DFS-based topo sort produces reverse postorder
                std::reverse( result.begin(), result.end() );

                // Fire visitor hooks
                if constexpr( requires {
                                  visitor.on_rank( knots.front(), std::size_t{} );
                              } )
                {
                    for( std::size_t i = 0; i < result.size(); ++i )
                    {
                        visitor.on_rank( result[i], i );
                    }
                }

                return result;
            };

            // ── Dispatch: DenseGraph uses flat vector, otherwise hash map ──

            if constexpr( web::DenseGraph<G> )
            {
                std::vector<State> color( graph.dense_size(), State::White );

                return run(
                    [&]( Knot k ) -> State
                    {
                        return color[graph.dense_id( k )];
                    },
                    [&]( Knot k, State s )
                    {
                        color[graph.dense_id( k )] = s;
                    }
                );
            }
            else
            {
                std::unordered_map<Knot, State> color;

                return run(
                    [&]( Knot k ) -> State
                    {
                        return color[k];
                    },
                    [&]( Knot k, State s )
                    {
                        color[k] = s;
                    }
                );
            }
        }

    }    // namespace detail

    // ── Public API ──────────────────────────────────────────────────────

    // Without visitor
    template<typename Strategy = Kahn,
             web::Graph G>
    [[nodiscard]]
    out::Put<std::vector<typename G::knot_type>,
             out::Error>
    rank( const G& graph )
    {
        logger::trace( logger::tag( "walk.algo" ),
                       "rank() topological sort knots={}",
                       graph.knots().size() );
        RankNoVisitor nv;
        if constexpr( std::is_same_v<Strategy, Kahn> )
        {
            return detail::rank_kahn( graph, nv );
        }
        else if constexpr( std::is_same_v<Strategy, DfsOrder> )
        {
            return detail::rank_dfs( graph, nv );
        }
        else
        {
            static_assert( std::is_same_v<Strategy, Kahn>, "Unknown rank strategy" );
        }
    }

    // With visitor
    template<typename Strategy = Kahn,
             web::Graph G,
             typename Vis>
    [[nodiscard]]
    out::Put<std::vector<typename G::knot_type>,
             out::Error>
    rank( const G& graph,
          Vis&     visitor )
    {
        if constexpr( std::is_same_v<Strategy, Kahn> )
        {
            return detail::rank_kahn( graph, visitor );
        }
        else if constexpr( std::is_same_v<Strategy, DfsOrder> )
        {
            return detail::rank_dfs( graph, visitor );
        }
        else
        {
            static_assert( std::is_same_v<Strategy, Kahn>, "Unknown rank strategy" );
        }
    }

}    // namespace walk
