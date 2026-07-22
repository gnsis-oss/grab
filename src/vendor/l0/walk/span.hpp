#pragma once
// ┌──────────────────────────────────────────────────────────────────────┐
// │  walk/span.h -- minimum spanning tree algorithms                    │
// └──────────────────────────────────────────────────────────────────────┘
//
// Strategies:
//   Prim (default)  -- priority-queue based (uses heap::Heap)
//   Kruskal         -- edge-sorting + union-find (uses walk::detail::Kin)
//
// Returns MST edges as pairs of knots, or out::Error::not_found if the
// graph is disconnected. Empty/single-knot graphs return empty vector.
// Requires web::Weighted<Edge> -- enforced by requires clause.
//
// Visitor hooks (optional):
//   on_edge_pick(from, to) -- edge added to MST
//   on_edge_skip(from, to) -- edge skipped (would create cycle)

#include <algorithm>
#include <heap/heap.hpp>
#include <out/put.hpp>
#include <tag/tag.hpp>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <walk/detail/union_find.hpp>
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
    struct SpanEntry
    {
            tag::Id<64>  id;        // knot identity for heap indexing
            Knot         knot;
            Knot         parent;    // knot that connected us to the tree (nil if root)
            Weight<Edge> cost;      // edge weight connecting us
    };

    // ── Prim's algorithm ────────────────────────────────────────────────

    template<web::Graph G,
             typename Vis>
    [[nodiscard]]
    out::Put<std::vector<std::pair<typename G::knot_type,
                                   typename G::knot_type>>,
             out::Error>
    span_prim( const G& graph,
               Vis&     visitor )
    {
        using Knot  = G::knot_type;
        using Edge  = G::edge_type;
        using Entry = SpanEntry<Knot, Edge>;
        using W     = Weight<Edge>;

        auto knots  = graph.knots();
        if( knots.empty() )
        {
            return std::vector<std::pair<Knot, Knot>>{};
        }

        std::unordered_set<Knot>           in_tree;
        std::vector<std::pair<Knot, Knot>> edges;
        edges.reserve( knots.size() - 1 );

        heap::Heap<Entry, heap::Min> pq;
        std::unordered_map<Knot, W>  best_cost;

        const Knot                   start = knots.front();
        [[maybe_unused]]
        auto init        = pq.add( Entry{ start.id(), start, Knot{}, W{} } );
        best_cost[start] = W{};

        while( !pq.is_empty() )
        {
            auto        pop_result = pq.pop();
            const Entry entry      = *pop_result.ok();

            if( in_tree.contains( entry.knot ) )
            {
                continue;
            }

            in_tree.insert( entry.knot );

            if( !entry.parent.nil() )
            {
                edges.push_back( { entry.parent, entry.knot } );
                if constexpr( requires {
                                  visitor.on_edge_pick( entry.parent, entry.knot );
                              } )
                {
                    visitor.on_edge_pick( entry.parent, entry.knot );
                }
            }

            for( const auto& adj : graph.out( entry.knot ) )
            {
                const Knot neighbor = adj.target;
                const W    w        = web::Trait<Edge>::weight( adj.data );

                if( in_tree.contains( neighbor ) )
                {
                    if constexpr( requires {
                                      visitor.on_edge_skip( entry.knot, neighbor );
                                  } )
                    {
                        visitor.on_edge_skip( entry.knot, neighbor );
                    }
                    continue;
                }

                auto it = best_cost.find( neighbor );
                if( it == best_cost.end() )
                {
                    best_cost[neighbor] = w;
                    [[maybe_unused]]
                    auto r = pq.add( Entry{ neighbor.id(), neighbor, entry.knot, w } );
                }
                else if( w < it->second )
                {
                    it->second = w;
                    [[maybe_unused]]
                    auto r = pq.rank( Entry{ neighbor.id(), neighbor, entry.knot, w } );
                }
            }
        }

        if( edges.size() != knots.size() - 1 )
        {
            return out::Error::not_found;
        }

        return edges;
    }

    // ── Kruskal's algorithm ─────────────────────────────────────────────

    template<web::Graph G,
             typename Vis>
    [[nodiscard]]
    out::Put<std::vector<std::pair<typename G::knot_type,
                                   typename G::knot_type>>,
             out::Error>
    span_kruskal( const G& graph,
                  Vis&     visitor )
    {
        using Knot = G::knot_type;
        using Edge = G::edge_type;
        using W    = Weight<Edge>;

        auto knots = graph.knots();
        if( knots.empty() )
        {
            return std::vector<std::pair<Knot, Knot>>{};
        }

        // Build knot-to-index map
        std::unordered_map<Knot, int> knot_index;
        for( std::size_t i = 0; i < knots.size(); ++i )
        {
            knot_index[knots[i]] = static_cast<int>( i );
        }

        // Collect all edges (each undirected edge appears once)
        struct WeightedEdge
        {
                Knot from;
                Knot to;
                W    weight;
        };

        std::vector<WeightedEdge>      all_edges;

        const std::unordered_set<Knot> visited_knots;
        for( const auto& u : knots )
        {
            for( const auto& adj : graph.out( u ) )
            {
                const Knot v = adj.target;
                // Only add each undirected edge once (u < v)
                if( u < v )
                {
                    all_edges.push_back(
                        WeightedEdge{ u, v, web::Trait<Edge>::weight( adj.data ) }
                    );
                }
            }
        }

        // Sort edges by weight
        std::sort( all_edges.begin(),
                   all_edges.end(),
                   []( const WeightedEdge& a, const WeightedEdge& b )
                   {
                       return a.weight < b.weight;
                   } );

        // Union-Find
        detail::Kin<>                      uf( knots.size() );
        std::vector<std::pair<Knot, Knot>> mst_edges;
        mst_edges.reserve( knots.size() - 1 );

        for( const auto& edge : all_edges )
        {
            const int ui = knot_index[edge.from];
            const int vi = knot_index[edge.to];

            if( !uf.same( ui, vi ) )
            {
                uf.bond( ui, vi );
                mst_edges.push_back( { edge.from, edge.to } );

                if constexpr( requires { visitor.on_edge_pick( edge.from, edge.to ); } )
                {
                    visitor.on_edge_pick( edge.from, edge.to );
                }

                if( mst_edges.size() == knots.size() - 1 )
                {
                    break;
                }
            }
            else
            {
                if constexpr( requires { visitor.on_edge_skip( edge.from, edge.to ); } )
                {
                    visitor.on_edge_skip( edge.from, edge.to );
                }
            }
        }

        if( mst_edges.size() != knots.size() - 1 )
        {
            return out::Error::not_found;
        }

        return mst_edges;
    }

}    // namespace walk::detail

// ── heap::Trait specialization ───────────────────────────────────────

template<typename Knot, typename Edge>
struct heap::Trait<walk::detail::SpanEntry<Knot, Edge>>
{
        static tag::Id<64>
        id( const walk::detail::SpanEntry<Knot,
                                          Edge>& e )
        {
            return e.id;
        }

        static auto
        key( const walk::detail::SpanEntry<Knot,
                                           Edge>& e )
        {
            return e.cost;
        }
};

// ── Public API ──────────────────────────────────────────────────────

namespace walk
{

    struct SpanNoVisitor
    {
    };

    // Without visitor
    template<typename Strategy = Prim,
             web::Graph G>
    requires web::Weighted<typename G::edge_type>
    [[nodiscard]]
    out::Put<std::vector<std::pair<typename G::knot_type,
                                   typename G::knot_type>>,
             out::Error>
    span( const G& graph )
    {
        SpanNoVisitor nv;
        if constexpr( std::is_same_v<Strategy, Prim> )
        {
            return detail::span_prim( graph, nv );
        }
        else if constexpr( std::is_same_v<Strategy, Kruskal> )
        {
            return detail::span_kruskal( graph, nv );
        }
        else
        {
            static_assert( std::is_same_v<Strategy, Prim>, "Unknown span strategy" );
        }
    }

    // With visitor
    template<typename Strategy = Prim,
             web::Graph G,
             typename Vis>
    requires web::Weighted<typename G::edge_type>
    [[nodiscard]]
    out::Put<std::vector<std::pair<typename G::knot_type,
                                   typename G::knot_type>>,
             out::Error>
    span( const G& graph,
          Vis&     visitor )
    {
        if constexpr( std::is_same_v<Strategy, Prim> )
        {
            return detail::span_prim( graph, visitor );
        }
        else if constexpr( std::is_same_v<Strategy, Kruskal> )
        {
            return detail::span_kruskal( graph, visitor );
        }
        else
        {
            static_assert( std::is_same_v<Strategy, Prim>, "Unknown span strategy" );
        }
    }

}    // namespace walk
