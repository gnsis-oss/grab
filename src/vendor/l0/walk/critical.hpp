#pragma once
// ┌──────────────────────────────────────────────────────────────────────┐
// │  walk/critical.hpp -- longest-weighted-path in a DAG                │
// └──────────────────────────────────────────────────────────────────────┘
//
// Directed-only (OneWay) — compile error on AnyWay (longest path on
// undirected is NP-hard).
//
// Returns CriticalPath{path, total}. Errors:
//   out::Error::not_found -- graph is empty
//   out::Error::stuck     -- graph contains a cycle (topological sort failed)
//
// Single-knot graph: path = {knot}, total = Weight{}.

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <out/put.hpp>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
#include <walk/rank.hpp>
#include <web/concept.hpp>
#include <web/knot.hpp>
#include <web/web.hpp>

namespace walk
{

    template<typename Weight>
    struct CriticalPath
    {
            std::vector<web::Knot> path;
            Weight                 total;
    };

    template<typename P,
             typename E,
             typename WeightFn>
    requires std::same_as<P,
                          web::OneWay> &&
             std::invocable<WeightFn&,
                            const E&>
    [[nodiscard]]
    out::Put<CriticalPath<std::invoke_result_t<WeightFn&,
                                               const E&>>,
             out::Error>
    critical( const web::Web<P,
                             E>& g,
              const WeightFn&    weight )
    {
        using W = std::invoke_result_t<WeightFn&, const E&>;

        if( g.knots().empty() )
        {
            return out::Error::not_found;
        }

        auto rank_result = rank( g );
        if( !rank_result )
        {
            return rank_result.error();
        }
        const auto&                              order = *rank_result.ok();

        std::unordered_map<web::Knot, W>         dist;
        std::unordered_map<web::Knot, web::Knot> pred;
        for( const auto& kn : order )
        {
            dist[kn] = W{};
        }

        for( const auto& u : order )
        {
            auto span = g.out( u );
            for( const auto& adj : span )
            {
                const web::Knot v         = adj.target;
                const W         w         = weight( adj.data );
                const W         candidate = dist[u] + w;
                if( candidate > dist[v] )
                {
                    dist[v] = candidate;
                    pred[v] = u;
                }
            }
        }

        // Find argmax-dist endpoint.
        web::Knot end  = order.front();
        W         best = dist[end];
        for( const auto& kn : order )
        {
            if( dist[kn] > best )
            {
                best = dist[kn];
                end  = kn;
            }
        }

        // Reconstruct path.
        std::vector<web::Knot> path;
        web::Knot              cur = end;
        path.push_back( cur );
        while( pred.contains( cur ) )
        {
            cur = pred.at( cur );
            path.push_back( cur );
        }
        std::ranges::reverse( path );

        return CriticalPath<W>{ std::move( path ), best };
    }

}    // namespace walk
