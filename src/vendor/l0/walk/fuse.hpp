#pragma once
// ┌──────────────────────────────────────────────────────────────────────┐
// │  walk/fuse.h -- graph union with edge-conflict resolution           │
// └──────────────────────────────────────────────────────────────────────┘
//
// Unions two graphs `a` and `b`. Knot union is set-union on Knot identity.
// Edge conflict (same (u,v) in both with different data) is resolved by:
//   - default overload: left-biased — `a`'s edge data wins
//   - merger overload:  caller-supplied E merge(const E&, const E&)

#include <concepts>
#include <type_traits>
#include <utility>
#include <web/concept.hpp>
#include <web/knot.hpp>
#include <web/web.hpp>

namespace walk
{

    namespace detail
    {

        // Merge two graphs into `out`. `prefer_a` (when true) keeps `a`'s edge
        // data on conflict; otherwise calls `merge(a_data, b_data)`.
        template<typename P,
                 typename E,
                 typename Merger>
        void
        fuse_merge_into( web::Web<P,
                                  E>&       out,
                         const web::Web<P,
                                        E>& a,
                         const web::Web<P,
                                        E>& b,
                         bool               prefer_a,
                         const Merger&      merge )
        {
            // Copy knots from a then b. Duplicate adds are no-ops (web::Web::add
            // returns Busy, which we ignore).
            for( const auto& kn : a.knots() )
            {
                [[maybe_unused]]
                auto r = out.add( kn );
            }
            for( const auto& kn : b.knots() )
            {
                [[maybe_unused]]
                auto r = out.add( kn );
            }

            // Copy edges from a unconditionally (they win by default).
            for( const auto& from : a.knots() )
            {
                auto span = a.out( from );
                for( const auto& adj : span )
                {
                    if constexpr( std::is_void_v<E> )
                    {
                        [[maybe_unused]]
                        auto r = out.tie( from, adj );
                    }
                    else
                    {
                        [[maybe_unused]]
                        auto r = out.tie( from, adj.target, adj.data );
                    }
                }
            }

            // For b: add edges not in a, or merge on conflict.
            for( const auto& from : b.knots() )
            {
                auto b_span = b.out( from );
                for( const auto& b_adj : b_span )
                {
                    web::Knot to;
                    if constexpr( std::is_void_v<E> )
                    {
                        to = b_adj;
                    }
                    else
                    {
                        to = b_adj.target;
                    }

                    if( !a.has( from, to ) )
                    {
                        // b-only edge → add
                        if constexpr( std::is_void_v<E> )
                        {
                            [[maybe_unused]]
                            auto r = out.tie( from, to );
                        }
                        else
                        {
                            [[maybe_unused]]
                            auto r = out.tie( from, to, b_adj.data );
                        }
                        continue;
                    }

                    // Conflict: a already supplied this edge.
                    if( prefer_a )
                    {
                        continue;    // left-biased, a wins
                    }

                    // Merger overload: recompute via caller.
                    if constexpr( !std::is_void_v<E> )
                    {
                        auto a_span = a.out( from );
                        for( const auto& a_adj : a_span )
                        {
                            if( a_adj.target == to )
                            {
                                E merged = merge( a_adj.data, b_adj.data );
                                [[maybe_unused]]
                                auto rc = out.cut( from, to );
                                [[maybe_unused]]
                                auto rt = out.tie( from, to, merged );
                                break;
                            }
                        }
                    }
                }
            }
        }

        struct FuseNoMerge
        {
                template<typename E>
                E
                operator()( const E& x,
                            const E& /*unused*/ ) const
                {
                    return x;
                }
        };

    }    // namespace detail

    // Default overload — left-biased.
    template<typename P,
             typename E>
    [[nodiscard]]
    web::Web<P,
             E>
    fuse( const web::Web<P,
                         E>& a,
          const web::Web<P,
                         E>& b )
    {
        web::Web<P, E>            out;
        const detail::FuseNoMerge nm;
        detail::fuse_merge_into( out, a, b, true, nm );
        return out;
    }

    // Merger overload — caller controls conflict resolution.
    template<typename P,
             typename E,
             typename Merger>
    requires( !std::is_void_v<E> ) &&
            std::invocable<Merger&,
                           const E&,
                           const E&> &&
            std::convertible_to<std::invoke_result_t<Merger&,
                                                     const E&,
                                                     const E&>,
                                E>
    [[nodiscard]]
    web::Web<P,
             E>
    fuse( const web::Web<P,
                         E>& a,
          const web::Web<P,
                         E>& b,
          const Merger&      merge )
    {
        web::Web<P, E> out;
        detail::fuse_merge_into( out, a, b, false, merge );
        return out;
    }

}    // namespace walk
