#pragma once
// ┌──────────────────────────────────────────────────────────────────────┐
// │  walk/diff.h -- structural delta between two graphs                 │
// └──────────────────────────────────────────────────────────────────────┘
//
// Computes per-element differences between `before` and `after`:
//   added_knots, removed_knots, added_edges, removed_edges, changed_edges.
//
// Default edge comparator is walk::EdgeValueEq (uses operator==).
// Callers with semantic equality (CRDT timestamps, vector clocks, payload-
// only) supply their own comparator via the optional third argument.
//
// For E = void, changed_edges stays empty (no edge data to compare).

#include <concepts>
#include <functional>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <web/concept.hpp>
#include <web/knot.hpp>
#include <web/web.hpp>

namespace walk
{

    template<typename P, typename E>
    struct Diff
    {
            std::vector<web::Knot>                       added_knots;
            std::vector<web::Knot>                       removed_knots;
            std::vector<std::pair<web::Knot, web::Knot>> added_edges;
            std::vector<std::pair<web::Knot, web::Knot>> removed_edges;
            std::vector<std::pair<web::Knot, web::Knot>> changed_edges;
    };

    // Default edge comparator — value equality via operator==.
    // Named (not anonymous) so call-site choices are greppable.
    struct EdgeValueEq
    {
            template<typename E>
            [[nodiscard]]
            constexpr bool
            operator()( const E& x,
                        const E& y ) const noexcept( noexcept( x == y ) )
            {
                return x == y;
            }
    };

    template<typename P,
             typename E,
             typename EdgeEq = EdgeValueEq>
    requires std::is_void_v<E> || std::predicate<EdgeEq&,
                                                 const E&,
                                                 const E&>
    [[nodiscard]]
    Diff<P,
         E>
    diff( const web::Web<P,
                         E>& before,
          const web::Web<P,
                         E>& after,
          const EdgeEq&      eq = EdgeEq{} )
    {
        Diff<P, E>                          result;

        auto                                before_knots = before.knots();
        auto                                after_knots  = after.knots();
        const std::unordered_set<web::Knot> before_set( before_knots.begin(),
                                                        before_knots.end() );
        const std::unordered_set<web::Knot> after_set( after_knots.begin(),
                                                       after_knots.end() );

        for( const auto& kn : after_knots )
        {
            if( !before_set.contains( kn ) )
            {
                result.added_knots.push_back( kn );
            }
        }
        for( const auto& kn : before_knots )
        {
            if( !after_set.contains( kn ) )
            {
                result.removed_knots.push_back( kn );
            }
        }

        // Keep endpoint equality in the membership key. Hashes may collide;
        // distinct endpoint pairs must still produce distinct edge deltas.
        using EdgeSet = std::unordered_map<web::Knot, std::unordered_set<web::Knot>>;
        auto contains_edge = []( const EdgeSet& edges, web::Knot from, web::Knot to )
        {
            const auto found = edges.find( from );
            return found != edges.end() && found->second.contains( to );
        };

        // Collect `before` edges.
        EdgeSet                                      before_edge_set;
        std::vector<std::pair<web::Knot, web::Knot>> before_edges;

        for( const auto& from : before_knots )
        {
            auto out_span = before.out( from );
            for( const auto& adj : out_span )
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
                before_edge_set[from].insert( to );
                before_edges.emplace_back( from, to );
            }
        }

        // Walk `after` edges: classify as added, or (if also in before) compare data.
        EdgeSet after_edge_set;
        for( const auto& from : after_knots )
        {
            auto out_span = after.out( from );
            for( const auto& adj : out_span )
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
                after_edge_set[from].insert( to );

                if( !contains_edge( before_edge_set, from, to ) )
                {
                    result.added_edges.emplace_back( from, to );
                }
                else if constexpr( !std::is_void_v<E> )
                {
                    // Both sides have this edge; compare data.
                    auto before_span = before.out( from );
                    for( const auto& badj : before_span )
                    {
                        if( badj.target == to )
                        {
                            if( !eq( badj.data, adj.data ) )
                            {
                                result.changed_edges.emplace_back( from, to );
                            }
                            break;
                        }
                    }
                }
            }
        }

        for( const auto& [from, to] : before_edges )
        {
            if( !contains_edge( after_edge_set, from, to ) )
            {
                result.removed_edges.emplace_back( from, to );
            }
        }

        return result;
    }

}    // namespace walk
