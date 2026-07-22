#pragma once
// ┌──────────────────────────────────────────────────────────────────────┐
// │  walk/reduce.h -- quotient-graph contraction                        │
// └──────────────────────────────────────────────────────────────────────┘
//
// Collapses equivalent knots into a representative. Two input shapes:
//   - ByRep   (default, unmarked): caller provides rep: Knot -> Knot      [O(V)]
//   - ByEquiv (opt-in):            caller provides equiv: (Knot,Knot)->bool [O(V^2)]
// Self-loops created by rep(u) == rep(v) are dropped.
// On duplicate rewritten edges, the first-inserted wins (match fuse's default).

#include <concepts>
#include <cstddef>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
#include <walk/detail/union_find.hpp>
#include <walk/strategy.hpp>
#include <web/knot.hpp>
#include <web/web.hpp>

namespace walk
{

    template<typename P, typename E>
    struct Quotient
    {
            web::Web<P, E>                           graph;
            std::unordered_map<web::Knot, web::Knot> representative_of;
    };

    namespace detail
    {

        template<typename P,
                 typename E>
        Quotient<P,
                 E>
        reduce_build( const web::Web<P,
                                     E>&                   g,
                      const std::unordered_map<web::Knot,
                                               web::Knot>& rep_of )
        {
            Quotient<P, E> out;
            out.representative_of = rep_of;

            // Add each unique representative as a knot.
            for( const auto& [kn, r] : rep_of )
            {
                if( !out.graph.has( r ) )
                {
                    [[maybe_unused]]
                    auto add_r = out.graph.add( r );
                }
            }

            // Rewrite edges. Drop self-loops (rep(u) == rep(v)). First edge wins.
            for( const auto& from : g.knots() )
            {
                auto span = g.out( from );
                for( const auto& adj : span )
                {
                    web::Knot to;
                    if constexpr( std::is_void_v<E> )
                    {
                        to = adj;
                    }
                    else
                    {
                        to = adj.target;
                    }
                    const web::Knot rf = rep_of.at( from );
                    const web::Knot rt = rep_of.at( to );
                    if( rf == rt )
                    {
                        continue;    // self-loop drop
                    }
                    if( out.graph.has( rf, rt ) )
                    {
                        continue;    // first wins
                    }
                    if constexpr( std::is_void_v<E> )
                    {
                        [[maybe_unused]]
                        auto r = out.graph.tie( rf, rt );
                    }
                    else
                    {
                        [[maybe_unused]]
                        auto r = out.graph.tie( rf, rt, adj.data );
                    }
                }
            }

            return out;
        }

    }    // namespace detail

    // Default (ByRep) — unmarked overload.
    template<typename P,
             typename E,
             typename RepFn>
    requires std::invocable<RepFn&,
                            web::Knot> &&
             std::same_as<std::invoke_result_t<RepFn&,
                                               web::Knot>,
                          web::Knot>
    [[nodiscard]]
    Quotient<P,
             E>
    reduce( const web::Web<P,
                           E>& g,
            const RepFn&       rep )
    {
        std::unordered_map<web::Knot, web::Knot> rep_of;
        rep_of.reserve( g.size() );
        for( const auto& kn : g.knots() )
        {
            rep_of.emplace( kn, rep( kn ) );
        }
        return detail::reduce_build( g, rep_of );
    }

    // Opt-in (ByEquiv) — strategy-tagged overload.
    template<typename Strategy,
             typename P,
             typename E,
             typename EquivFn>
    requires std::same_as<Strategy,
                          ByEquiv> &&
             std::predicate<EquivFn&,
                            web::Knot,
                            web::Knot>
    [[nodiscard]]
    Quotient<P,
             E>
    reduce( const web::Web<P,
                           E>& g,
            const EquivFn&     equiv )
    {
        auto                               knots_vec = g.knots();
        std::unordered_map<web::Knot, int> idx;
        idx.reserve( knots_vec.size() );
        for( std::size_t i = 0; i < knots_vec.size(); ++i )
        {
            idx.emplace( knots_vec[i], static_cast<int>( i ) );
        }

        detail::Kin<detail::ByRank, detail::Halve> kin( knots_vec.size() );
        for( std::size_t i = 0; i < knots_vec.size(); ++i )
        {
            for( std::size_t j = i + 1; j < knots_vec.size(); ++j )
            {
                if( equiv( knots_vec[i], knots_vec[j] ) )
                {
                    kin.bond( static_cast<int>( i ), static_cast<int>( j ) );
                }
            }
        }

        // Pick smallest-Knot member per class as representative (deterministic).
        std::unordered_map<int, web::Knot> rep_for_root;
        for( std::size_t i = 0; i < knots_vec.size(); ++i )
        {
            const int root = kin.find( static_cast<int>( i ) );
            auto      it   = rep_for_root.find( root );
            if( it == rep_for_root.end() || knots_vec[i] < it->second )
            {
                rep_for_root[root] = knots_vec[i];
            }
        }

        std::unordered_map<web::Knot, web::Knot> rep_of;
        rep_of.reserve( knots_vec.size() );
        for( std::size_t i = 0; i < knots_vec.size(); ++i )
        {
            const int root = kin.find( static_cast<int>( i ) );
            rep_of.emplace( knots_vec[i], rep_for_root.at( root ) );
        }

        return detail::reduce_build( g, rep_of );
    }

}    // namespace walk
