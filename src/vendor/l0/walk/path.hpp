#pragma once
// ┌──────────────────────────────────────────────────────────────────────┐
// │  walk/path.h -- shortest path algorithms with strategy dispatch     │
// └──────────────────────────────────────────────────────────────────────┘
//
// Strategies:
//   Dijkstra (default)  -- single-source, non-negative weights
//   AStar               -- A* with heuristic, single-source single-target
//   BellmanFord         -- single-source, handles negative weights
//   DagPath             -- DAG shortest path via topological order
//   FloydWarshall       -- all-pairs shortest paths
//
// Visitor hooks for Dijkstra/AStar (optional):
//   on_find(knot)        -- knot first discovered
//   on_relax(from, to)   -- edge relaxed (shorter path found)
//   on_settle(knot)      -- knot finalized (popped from heap)
//
// Visitor hooks for AStar (additional):
//   on_prune(knot)       -- knot pruned (already settled)
//
// Visitor hooks for BellmanFord:
//   on_relax(from, to)   -- edge relaxed
//   on_settle(knot)      -- knot finalized
//
// Visitor hooks for FloydWarshall:
//   on_relax(i, j, k)    -- path through k improves i->j distance

#include <algorithm>
#include <heap/heap.hpp>
#include <limits>
#include <log/writer.hpp>
#include <out/detail/platform.hpp>
#include <out/put.hpp>
#include <queue>
#include <tag/tag.hpp>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <walk/strategy.hpp>
#include <web/concept.hpp>
#include <web/knot.hpp>
#include <web/trait.hpp>

// ── Internal detail ──────────────────────────────────────────────────

namespace walk::detail
{

    template<typename Edge>
    using Weight = decltype( web::Trait<Edge>::weight( std::declval<const Edge&>() ) );

    template<typename Knot, typename Edge>
    struct PathEntry
    {
            tag::Id<64>  id;    // knot identity for heap indexing
            Knot         knot;
            Weight<Edge> distance;
    };

    // PathEntry for A* (stores f = g + h)
    template<typename Knot, typename Edge>
    struct AStarEntry
    {
            tag::Id<64>  id;
            Knot         knot;
            Weight<Edge> g_cost;    // actual distance from start
            Weight<Edge> f_cost;    // g + h (priority key)
    };

}    // namespace walk::detail

// ── heap::Trait specializations ─────────────────────────────────────

template<typename Knot, typename Edge>
struct heap::Trait<walk::detail::PathEntry<Knot, Edge>>
{
        static tag::Id<64>
        id( const walk::detail::PathEntry<Knot,
                                          Edge>& e )
        {
            return e.id;
        }

        static auto
        key( const walk::detail::PathEntry<Knot,
                                           Edge>& e )
        {
            return e.distance;
        }
};

template<typename Knot, typename Edge>
struct heap::Trait<walk::detail::AStarEntry<Knot, Edge>>
{
        static tag::Id<64>
        id( const walk::detail::AStarEntry<Knot,
                                           Edge>& e )
        {
            return e.id;
        }

        static auto
        key( const walk::detail::AStarEntry<Knot,
                                            Edge>& e )
        {
            return e.f_cost;
        }
};

// ── Algorithms ──────────────────────────────────────────────────────

namespace walk
{

    // ── Dijkstra (default) ──────────────────────────────────────────────

    namespace detail
    {

        template<web::Graph G,
                 typename Vis>
        requires web::Weighted<typename G::edge_type>
        [[nodiscard]]
        out::Put<std::vector<typename G::knot_type>,
                 out::Error>
        path_dijkstra( const G&              graph,
                       typename G::knot_type start,
                       typename G::knot_type goal,
                       Vis&                  visitor )
        {
            using Knot  = G::knot_type;
            using Edge  = G::edge_type;
            using Entry = PathEntry<Knot, Edge>;
            using W     = Weight<Edge>;

            if( start == goal )
            {
                if( !graph.has( start ) )
                {
                    logger::error( logger::tag( "walk.algo" ),
                                   "path_dijkstra() start==goal but not in graph" );
                    return out::Error::not_found;
                }
                return std::vector<Knot>{ start };
            }
            if( !graph.has( start ) )
            {
                logger::error( logger::tag( "walk.algo" ),
                               "path_dijkstra() start not in graph" );
                return out::Error::not_found;
            }

            heap::Heap<Entry, heap::Min>   pq;
            std::unordered_map<Knot, W>    distances;
            std::unordered_map<Knot, Knot> parents;

            distances[start] = W{};
            [[maybe_unused]]
            auto init = pq.add( Entry{ start.id(), start, W{} } );

            if constexpr( requires { visitor.on_find( start ); } )
            {
                visitor.on_find( start );
            }

            while( !pq.is_empty() )
            {
                auto        pop_result = pq.pop();
                const Entry entry      = *pop_result.ok();

                if constexpr( requires { visitor.on_settle( entry.knot ); } )
                {
                    visitor.on_settle( entry.knot );
                }

                if( entry.knot == goal )
                {
                    std::vector<Knot> p;
                    Knot              cur = goal;
                    while( cur != start )
                    {
                        p.push_back( cur );
                        cur = parents.at( cur );
                    }
                    p.push_back( start );
                    std::reverse( p.begin(), p.end() );
                    return p;
                }

                {
                    const auto& adj_list = graph.out( entry.knot );
                    for( auto it_adj = adj_list.begin(); it_adj != adj_list.end();
                         ++it_adj )
                    {
                        const auto& adj = *it_adj;
                        // Prefetch next neighbor's adjacency data
                        auto        next_adj = std::next( it_adj );
                        if( next_adj != adj_list.end() )
                        {
                            SEED_PREFETCH( &( *next_adj ) );
                        }

                        const Knot neighbor = adj.target;
                        const W    edge_w   = web::Trait<Edge>::weight( adj.data );
                        const W    new_dist = entry.distance + edge_w;

                        auto       it       = distances.find( neighbor );
                        if( it == distances.end() || new_dist < it->second )
                        {
                            distances[neighbor] = new_dist;
                            parents[neighbor]   = entry.knot;

                            if constexpr( requires {
                                              visitor.on_relax( entry.knot, neighbor );
                                          } )
                            {
                                visitor.on_relax( entry.knot, neighbor );
                            }

                            if( pq.has( neighbor.id() ) )
                            {
                                [[maybe_unused]]
                                auto r = pq.rank(
                                    Entry{ neighbor.id(), neighbor, new_dist }
                                );
                            }
                            else
                            {
                                if constexpr( requires { visitor.on_find( neighbor ); } )
                                {
                                    visitor.on_find( neighbor );
                                }
                                [[maybe_unused]]
                                auto r =
                                    pq.add( Entry{ neighbor.id(), neighbor, new_dist } );
                            }
                        }
                    }
                }
            }

            logger::error( logger::tag( "walk.algo" ),
                           "path_dijkstra() no path found to goal" );
            return out::Error::not_found;
        }

        // ── A* ──────────────────────────────────────────────────────────────

        template<web::Reach G,
                 typename Heuristic,
                 typename Vis>
        requires web::Weighted<typename G::edge_type>
        [[nodiscard]]
        out::Put<std::vector<typename G::knot_type>,
                 out::Error>
        path_astar( const G&              graph,
                    typename G::knot_type start,
                    typename G::knot_type goal,
                    const Heuristic&      h,
                    Vis&                  visitor )
        {
            using Knot  = G::knot_type;
            using Edge  = G::edge_type;
            using Entry = AStarEntry<Knot, Edge>;
            using W     = Weight<Edge>;

            if( start == goal )
            {
                if constexpr( web::Graph<G> )
                {
                    if( !graph.has( start ) )
                    {
                        return out::Error::not_found;
                    }
                }
                return std::vector<Knot>{ start };
            }
            if constexpr( web::Graph<G> )
            {
                if( !graph.has( start ) )
                {
                    return out::Error::not_found;
                }
            }

            heap::Heap<Entry, heap::Min>   open;
            std::unordered_map<Knot, W>    g_scores;
            std::unordered_map<Knot, Knot> parents;
            std::unordered_set<Knot>       closed;

            const W                        h_start = static_cast<W>( h( start ) );
            g_scores[start]                        = W{};
            [[maybe_unused]]
            auto init = open.add( Entry{ start.id(), start, W{}, h_start } );

            if constexpr( requires { visitor.on_find( start ); } )
            {
                visitor.on_find( start );
            }

            while( !open.is_empty() )
            {
                auto        pop_result = open.pop();
                const Entry entry      = *pop_result.ok();

                if constexpr( requires { visitor.on_settle( entry.knot ); } )
                {
                    visitor.on_settle( entry.knot );
                }

                if( entry.knot == goal )
                {
                    std::vector<Knot> p;
                    Knot              cur = goal;
                    while( cur != start )
                    {
                        p.push_back( cur );
                        cur = parents.at( cur );
                    }
                    p.push_back( start );
                    std::reverse( p.begin(), p.end() );
                    return p;
                }

                closed.insert( entry.knot );

                for( const auto& adj : graph.out( entry.knot ) )
                {
                    const Knot neighbor = adj.target;

                    if( closed.contains( neighbor ) )
                    {
                        if constexpr( requires { visitor.on_prune( neighbor ); } )
                        {
                            visitor.on_prune( neighbor );
                        }
                        continue;
                    }

                    const W edge_w = web::Trait<Edge>::weight( adj.data );
                    const W new_g  = entry.g_cost + edge_w;
                    const W new_f  = new_g + static_cast<W>( h( neighbor ) );

                    auto    it     = g_scores.find( neighbor );
                    if( it == g_scores.end() || new_g < it->second )
                    {
                        g_scores[neighbor] = new_g;
                        parents[neighbor]  = entry.knot;

                        if constexpr( requires {
                                          visitor.on_relax( entry.knot, neighbor );
                                      } )
                        {
                            visitor.on_relax( entry.knot, neighbor );
                        }

                        if( open.has( neighbor.id() ) )
                        {
                            [[maybe_unused]]
                            auto r = open.rank(
                                Entry{ neighbor.id(), neighbor, new_g, new_f }
                            );
                        }
                        else
                        {
                            if constexpr( requires { visitor.on_find( neighbor ); } )
                            {
                                visitor.on_find( neighbor );
                            }
                            [[maybe_unused]]
                            auto r = open.add(
                                Entry{ neighbor.id(), neighbor, new_g, new_f }
                            );
                        }
                    }
                }
            }

            return out::Error::not_found;
        }

        // ── Bellman-Ford ────────────────────────────────────────────────────

        template<typename Knot, typename Edge>
        struct BellmanFordResult
        {
                std::unordered_map<Knot, Weight<Edge>> distances;
                std::unordered_map<Knot, Knot>         parents;
                bool                                   negative_cycle = false;
        };

        template<web::Graph G,
                 typename Vis>
        requires web::Weighted<typename G::edge_type>
        [[nodiscard]]
        BellmanFordResult<typename G::knot_type,
                          typename G::edge_type>
        path_bellman_ford( const G&              graph,
                           typename G::knot_type start,
                           Vis&                  visitor )
        {
            using Knot = G::knot_type;
            using Edge = G::edge_type;
            using W    = Weight<Edge>;

            BellmanFordResult<Knot, Edge> result;
            auto                          knots = graph.knots();

            if( knots.empty() || !graph.has( start ) )
            {
                return result;
            }

            // Initialize distances
            const W inf = std::numeric_limits<W>::max();
            for( const auto& k : knots )
            {
                result.distances[k] = inf;
            }
            result.distances[start]   = W{};

            const std::size_t n_knots = knots.size();

            // Relax all edges n_knots-1 times
            for( std::size_t i = 0; i < n_knots - 1; ++i )
            {
                bool changed = false;
                for( const auto& u : knots )
                {
                    if( result.distances[u] == inf )
                    {
                        continue;
                    }
                    for( const auto& adj : graph.out( u ) )
                    {
                        const Knot v        = adj.target;
                        const W    edge_w   = web::Trait<Edge>::weight( adj.data );
                        const W    new_dist = result.distances[u] + edge_w;
                        if( new_dist < result.distances[v] )
                        {
                            result.distances[v] = new_dist;
                            result.parents[v]   = u;
                            changed             = true;

                            if constexpr( requires { visitor.on_relax( u, v ); } )
                            {
                                visitor.on_relax( u, v );
                            }
                        }
                    }
                }
                if( !changed )
                {
                    break;    // Early exit if no relaxation occurred
                }
            }

            // Check for negative cycles on the Vth pass
            for( const auto& u : knots )
            {
                if( result.distances[u] == inf )
                {
                    continue;
                }
                for( const auto& adj : graph.out( u ) )
                {
                    const Knot v      = adj.target;
                    const W    edge_w = web::Trait<Edge>::weight( adj.data );
                    if( result.distances[u] + edge_w < result.distances[v] )
                    {
                        result.negative_cycle = true;
                        return result;
                    }
                }
            }

            if constexpr( requires { visitor.on_settle( start ); } )
            {
                for( const auto& k : knots )
                {
                    if( result.distances[k] != inf )
                    {
                        visitor.on_settle( k );
                    }
                }
            }

            return result;
        }

        // ── Floyd-Warshall ──────────────────────────────────────────────────

        template<typename Knot, typename Edge>
        struct FloydResult
        {
                std::vector<Knot>         knots;    // ordered knot list
                std::vector<Weight<Edge>> dist;     // n_knots x n_knots flat matrix
                std::unordered_map<Knot, std::size_t> index;    // knot -> index

                [[nodiscard]]
                Weight<Edge>
                distance( Knot from,
                          Knot to ) const
                {
                    auto fi = index.find( from );
                    auto ti = index.find( to );
                    if( fi == index.end() || ti == index.end() )
                    {
                        return std::numeric_limits<Weight<Edge>>::max();
                    }
                    return dist[( fi->second * knots.size() ) + ti->second];
                }
        };

        template<web::Graph G,
                 typename Vis>
        requires web::Weighted<typename G::edge_type>
        [[nodiscard]]
        FloydResult<typename G::knot_type,
                    typename G::edge_type>
        path_floyd( const G& graph,
                    Vis&     visitor )
        {
            using Knot = G::knot_type;
            using Edge = G::edge_type;
            using W    = Weight<Edge>;

            FloydResult<Knot, Edge> result;
            result.knots              = graph.knots();

            const std::size_t n_knots = result.knots.size();
            if( n_knots == 0 )
            {
                return result;
            }

            // Build index map
            for( std::size_t i = 0; i < n_knots; ++i )
            {
                result.index[result.knots[i]] = i;
            }

            // Initialize distance matrix
            const W inf = std::numeric_limits<W>::max();
            result.dist.assign( n_knots * n_knots, inf );

            // Distance to self is 0
            for( std::size_t i = 0; i < n_knots; ++i )
            {
                result.dist[( i * n_knots ) + i] = W{};
            }

            // Fill in edges
            for( std::size_t i = 0; i < n_knots; ++i )
            {
                const Knot u = result.knots[i];
                for( const auto& adj : graph.out( u ) )
                {
                    const Knot v  = adj.target;
                    const W    w  = web::Trait<Edge>::weight( adj.data );
                    auto       vi = result.index[v];
                    if( w < result.dist[( i * n_knots ) + vi] )
                    {
                        result.dist[( i * n_knots ) + vi] = w;
                    }
                }
            }

            // Floyd-Warshall main loop
            for( std::size_t k = 0; k < n_knots; ++k )
            {
                for( std::size_t i = 0; i < n_knots; ++i )
                {
                    for( std::size_t j = 0; j < n_knots; ++j )
                    {
                        const W d_ik = result.dist[( i * n_knots ) + k];
                        const W d_kj = result.dist[( k * n_knots ) + j];
                        if( d_ik != inf && d_kj != inf )
                        {
                            const W via_k = d_ik + d_kj;
                            if( via_k < result.dist[( i * n_knots ) + j] )
                            {
                                result.dist[( i * n_knots ) + j] = via_k;

                                if constexpr( requires {
                                                  visitor.on_relax( result.knots[i],
                                                                    result.knots[j],
                                                                    result.knots[k] );
                                              } )
                                {
                                    visitor.on_relax( result.knots[i],
                                                      result.knots[j],
                                                      result.knots[k] );
                                }
                            }
                        }
                    }
                }
            }

            return result;
        }

        // ── DAG shortest path ───────────────────────────────────────────────
        // DagPath only works correctly on directed acyclic graphs.

        template<web::Graph G,
                 typename Vis>
        requires web::Weighted<typename G::edge_type>
        [[nodiscard]]
        out::Put<std::vector<typename G::knot_type>,
                 out::Error>
        path_dag( const G&              graph,
                  typename G::knot_type start,
                  typename G::knot_type goal,
                  Vis&                  visitor )
        {
            using Knot = G::knot_type;
            using Edge = G::edge_type;
            using W    = Weight<Edge>;

            if( start == goal )
            {
                if( !graph.has( start ) )
                {
                    return out::Error::not_found;
                }
                return std::vector<Knot>{ start };
            }
            if( !graph.has( start ) )
            {
                return out::Error::not_found;
            }

            // Get topological order using Kahn's algorithm inline
            auto                                  knots = graph.knots();
            std::unordered_map<Knot, std::size_t> in_degree;
            for( const auto& k : knots )
            {
                in_degree[k] = 0;
            }
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
                    in_degree[neighbor]++;
                }
            }

            std::queue<Knot> queue;
            for( const auto& k : knots )
            {
                if( in_degree.at( k ) == 0 )
                {
                    queue.push( k );
                }
            }

            std::vector<Knot> topo_order;
            topo_order.reserve( knots.size() );
            while( !queue.empty() )
            {
                const Knot k = queue.front();
                queue.pop();
                topo_order.push_back( k );
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
                    if( --in_degree[neighbor] == 0 )
                    {
                        queue.push( neighbor );
                    }
                }
            }

            if( topo_order.size() != knots.size() )
            {
                return out::Error::stuck;    // cycle detected -- not a DAG
            }

                                             // Relax edges in topological order
            const W                        inf = std::numeric_limits<W>::max();
            std::unordered_map<Knot, W>    distances;
            std::unordered_map<Knot, Knot> parents;

            for( const auto& k : knots )
            {
                distances[k] = inf;
            }
            distances[start] = W{};

            for( const auto& u : topo_order )
            {
                if( distances[u] == inf )
                {
                    continue;
                }

                if constexpr( requires { visitor.on_settle( u ); } )
                {
                    visitor.on_settle( u );
                }

                for( const auto& adj : graph.out( u ) )
                {
                    const Knot v        = adj.target;
                    const W    edge_w   = web::Trait<Edge>::weight( adj.data );
                    const W    new_dist = distances[u] + edge_w;
                    if( new_dist < distances[v] )
                    {
                        distances[v] = new_dist;
                        parents[v]   = u;

                        if constexpr( requires { visitor.on_relax( u, v ); } )
                        {
                            visitor.on_relax( u, v );
                        }
                    }
                }
            }

            // Reconstruct path
            if( distances[goal] == inf )
            {
                return out::Error::not_found;
            }

            std::vector<Knot> p;
            Knot              cur = goal;
            while( cur != start )
            {
                p.push_back( cur );
                cur = parents.at( cur );
            }
            p.push_back( start );
            std::reverse( p.begin(), p.end() );
            return p;
        }

    }    // namespace detail

    // ── Public API: path<Strategy>() ────────────────────────────────────

    // Default no-visitor
    struct PathNoVisitor
    {
    };

    // ── Dijkstra (default) ──────────────────────────────────────────────

    // Without visitor: path(graph, start, goal)
    template<typename Strategy = Dijkstra,
             web::Graph G>
    requires web::Weighted<typename G::edge_type> && ( std::is_same_v<Strategy,
                                                                      Dijkstra> )
    [[nodiscard]]
    out::Put<std::vector<typename G::knot_type>,
             out::Error>
    path( const G&              graph,
          typename G::knot_type start,
          typename G::knot_type goal )
    {
        logger::trace( logger::tag( "walk.algo" ),
                       "path() Dijkstra knots={}",
                       graph.knots().size() );
        PathNoVisitor nv;
        return detail::path_dijkstra( graph, start, goal, nv );
    }

    // With visitor: path(graph, start, goal, visitor)
    template<typename Strategy = Dijkstra,
             web::Graph G,
             typename Vis>
    requires web::Weighted<typename G::edge_type> &&
             ( std::is_same_v<Strategy,
                              Dijkstra> ) &&
             ( !std::is_same_v<std::decay_t<Vis>,
                               typename G::knot_type> )
    [[nodiscard]]
    out::Put<std::vector<typename G::knot_type>,
             out::Error>
    path( const G&              graph,
          typename G::knot_type start,
          typename G::knot_type goal,
          Vis&                  visitor )
    {
        return detail::path_dijkstra( graph, start, goal, visitor );
    }

    // ── A* ──────────────────────────────────────────────────────────────

    // Without visitor: path<AStar>(graph, start, goal, heuristic)
    template<typename Strategy,
             web::Reach G,
             typename Heuristic>
    requires web::Weighted<typename G::edge_type> && ( std::is_same_v<Strategy,
                                                                      AStar> )
    [[nodiscard]]
    out::Put<std::vector<typename G::knot_type>,
             out::Error>
    path( const G&              graph,
          typename G::knot_type start,
          typename G::knot_type goal,
          Heuristic&&           h )
    {
        PathNoVisitor nv;
        return detail::path_astar( graph,
                                   start,
                                   goal,
                                   std::forward<Heuristic>( h ),
                                   nv );
    }

    // With visitor: path<AStar>(graph, start, goal, heuristic, visitor)
    template<typename Strategy,
             web::Reach G,
             typename Heuristic,
             typename Vis>
    requires web::Weighted<typename G::edge_type> && ( std::is_same_v<Strategy,
                                                                      AStar> )
    [[nodiscard]]
    out::Put<std::vector<typename G::knot_type>,
             out::Error>
    path( const G&              graph,
          typename G::knot_type start,
          typename G::knot_type goal,
          Heuristic&&           h,
          Vis&                  visitor )
    {
        return detail::path_astar( graph,
                                   start,
                                   goal,
                                   std::forward<Heuristic>( h ),
                                   visitor );
    }

    // ── Bellman-Ford ────────────────────────────────────────────────────

    // Without visitor: path<BellmanFord>(graph, start)
    template<typename Strategy,
             web::Graph G>
    requires web::Weighted<typename G::edge_type> && ( std::is_same_v<Strategy,
                                                                      BellmanFord> )
    [[nodiscard]]
    detail::BellmanFordResult<typename G::knot_type,
                              typename G::edge_type>
    path( const G&              graph,
          typename G::knot_type start )
    {
        PathNoVisitor nv;
        return detail::path_bellman_ford( graph, start, nv );
    }

    // With visitor: path<BellmanFord>(graph, start, visitor)
    template<typename Strategy,
             web::Graph G,
             typename Vis>
    requires web::Weighted<typename G::edge_type> && ( std::is_same_v<Strategy,
                                                                      BellmanFord> )
    [[nodiscard]]
    detail::BellmanFordResult<typename G::knot_type,
                              typename G::edge_type>
    path( const G&              graph,
          typename G::knot_type start,
          Vis&                  visitor )
    {
        return detail::path_bellman_ford( graph, start, visitor );
    }

    // ── Floyd-Warshall ──────────────────────────────────────────────────

    // Without visitor: path<FloydWarshall>(graph)
    template<typename Strategy,
             web::Graph G>
    requires web::Weighted<typename G::edge_type> && ( std::is_same_v<Strategy,
                                                                      FloydWarshall> )
    [[nodiscard]]
    detail::FloydResult<typename G::knot_type,
                        typename G::edge_type>
    path( const G& graph )
    {
        PathNoVisitor nv;
        return detail::path_floyd( graph, nv );
    }

    // With visitor: path<FloydWarshall>(graph, visitor)
    template<typename Strategy,
             web::Graph G,
             typename Vis>
    requires web::Weighted<typename G::edge_type> && ( std::is_same_v<Strategy,
                                                                      FloydWarshall> )
    [[nodiscard]]
    detail::FloydResult<typename G::knot_type,
                        typename G::edge_type>
    path( const G& graph,
          Vis&     visitor )
    {
        return detail::path_floyd( graph, visitor );
    }

    // ── DAG shortest path ───────────────────────────────────────────────
    // DagPath only works correctly on directed acyclic graphs.

    // Without visitor: path<DagPath>(graph, start, goal)
    template<typename Strategy,
             web::Graph G>
    requires web::Weighted<typename G::edge_type> && ( std::is_same_v<Strategy,
                                                                      DagPath> )
    [[nodiscard]]
    out::Put<std::vector<typename G::knot_type>,
             out::Error>
    path( const G&              graph,
          typename G::knot_type start,
          typename G::knot_type goal )
    {
        PathNoVisitor nv;
        return detail::path_dag( graph, start, goal, nv );
    }

    // With visitor: path<DagPath>(graph, start, goal, visitor)
    template<typename Strategy,
             web::Graph G,
             typename Vis>
    requires web::Weighted<typename G::edge_type> && ( std::is_same_v<Strategy,
                                                                      DagPath> )
    [[nodiscard]]
    out::Put<std::vector<typename G::knot_type>,
             out::Error>
    path( const G&              graph,
          typename G::knot_type start,
          typename G::knot_type goal,
          Vis&                  visitor )
    {
        return detail::path_dag( graph, start, goal, visitor );
    }

}    // namespace walk
