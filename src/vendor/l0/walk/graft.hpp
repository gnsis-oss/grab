#pragma once
// ┌──────────────────────────────────────────────────────────────────────┐
// │  walk/graft.h -- subgraph attachment                                │
// └──────────────────────────────────────────────────────────────────────┘
//
// Strategies:
//   AttachRoots (default) -- each root of `sub` gets an edge from attach_point;
//                           attach_point remains in the graph
//   Splice                -- attach_point is removed; its in-edges reroute to
//                           sub's roots, its out-edges reroute from sub's leaves
//
// Sub's knots are always remapped to fresh ids (avoids id collisions with host).

#include <concepts>
#include <cstdint>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
#include <walk/strategy.hpp>
#include <web/knot.hpp>
#include <web/web.hpp>

namespace walk
{

    namespace detail
    {

        // Allocate a fresh knot id not used in `host`. Starts from `next` and
        // skips any id already present.
        template<typename P,
                 typename E>
        web::Knot
        fresh_id( const web::Web<P,
                                 E>& host,
                  std::uint64_t&     next )
        {
            while( host.has( web::Knot{ next } ) )
            {
                ++next;
            }
            web::Knot id{ next };
            ++next;
            return id;
        }

        // Copy `sub` into `host_out`, returning a map sub_knot -> host_knot.
        template<typename P,
                 typename E>
        std::unordered_map<web::Knot,
                           web::Knot>
        splice_sub_into( web::Web<P,
                                  E>&       host_out,
                         const web::Web<P,
                                        E>& sub )
        {
            std::unordered_map<web::Knot, web::Knot> remap;
            std::uint64_t                            next = 1;

            for( const auto& kn : sub.knots() )
            {
                const web::Knot id = fresh_id( host_out, next );
                [[maybe_unused]]
                auto r = host_out.add( id );
                remap.emplace( kn, id );
            }
            for( const auto& from : sub.knots() )
            {
                auto span = sub.out( from );
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
                    if constexpr( std::is_void_v<E> )
                    {
                        [[maybe_unused]]
                        auto r = host_out.tie( remap.at( from ), remap.at( to ) );
                    }
                    else
                    {
                        [[maybe_unused]]
                        auto r =
                            host_out.tie( remap.at( from ), remap.at( to ), adj.data );
                    }
                }
            }
            return remap;
        }

        // Roots of `sub`: knots with no incoming edges (OneWay).
        // For AnyWay, fall back to using the first knot as the only root.
        template<typename P,
                 typename E>
        std::vector<web::Knot>
        sub_roots( const web::Web<P,
                                  E>& sub )
        {
            std::vector<web::Knot> out;
            if constexpr( std::is_same_v<P, web::OneWay> )
            {
                for( const auto& kn : sub.knots() )
                {
                    if( sub.in( kn ).empty() )
                    {
                        out.push_back( kn );
                    }
                }
                if( out.empty() && !sub.knots().empty() )
                {
                    out.push_back( sub.knots().front() );
                }
            }
            else
            {
                if( !sub.knots().empty() )
                {
                    out.push_back( sub.knots().front() );
                }
            }
            return out;
        }

        // Leaves of `sub`: knots with no outgoing edges (OneWay).
        template<typename P,
                 typename E>
        std::vector<web::Knot>
        sub_leaves( const web::Web<P,
                                   E>& sub )
        {
            std::vector<web::Knot> out;
            if constexpr( std::is_same_v<P, web::OneWay> )
            {
                for( const auto& kn : sub.knots() )
                {
                    if( sub.out( kn ).empty() )
                    {
                        out.push_back( kn );
                    }
                }
                if( out.empty() && !sub.knots().empty() )
                {
                    out.push_back( sub.knots().back() );
                }
            }
            else
            {
                if( !sub.knots().empty() )
                {
                    out.push_back( sub.knots().back() );
                }
            }
            return out;
        }

    }    // namespace detail

    // AttachRoots (default): attach sub's roots to attach_point.
    template<typename P,
             typename E>
    [[nodiscard]]
    web::Web<P,
             E>
    graft( web::Web<P,
                    E>        host,
           const web::Web<P,
                          E>& sub,
           web::Knot          attach_point )
    {
        auto remap = detail::splice_sub_into( host, sub );
        for( const auto& root : detail::sub_roots( sub ) )
        {
            if constexpr( std::is_void_v<E> )
            {
                [[maybe_unused]]
                auto r = host.tie( attach_point, remap.at( root ) );
            }
            else
            {
                [[maybe_unused]]
                auto r = host.tie( attach_point, remap.at( root ), E{} );
            }
        }
        return host;
    }

    // Splice: remove attach_point; reroute its in-edges to sub's roots and
    // its out-edges from sub's leaves.
    template<typename Strategy,
             typename P,
             typename E>
    requires std::same_as<Strategy,
                          Splice>
    [[nodiscard]]
    web::Web<P,
             E>
    graft( web::Web<P,
                    E>        host,
           const web::Web<P,
                          E>& sub,
           web::Knot          attach_point )
    {
        if constexpr( std::is_void_v<E> )
        {
            std::vector<web::Knot> incoming;
            std::vector<web::Knot> outgoing;
            for( const auto& adj : host.in( attach_point ) )
            {
                incoming.push_back( adj );
            }
            for( const auto& adj : host.out( attach_point ) )
            {
                outgoing.push_back( adj );
            }

            [[maybe_unused]]
            auto rr     = host.rid( attach_point );

            auto remap  = detail::splice_sub_into( host, sub );
            auto roots  = detail::sub_roots( sub );
            auto leaves = detail::sub_leaves( sub );

            for( const auto& src : incoming )
            {
                for( const auto& root : roots )
                {
                    [[maybe_unused]]
                    auto r = host.tie( src, remap.at( root ) );
                }
            }
            for( const auto& dst : outgoing )
            {
                for( const auto& leaf : leaves )
                {
                    [[maybe_unused]]
                    auto r = host.tie( remap.at( leaf ), dst );
                }
            }
            return host;
        }
        else
        {
            std::vector<std::pair<web::Knot, E>> incoming;
            std::vector<std::pair<web::Knot, E>> outgoing;
            for( const auto& adj : host.in( attach_point ) )
            {
                incoming.emplace_back( adj.target, adj.data );
            }
            for( const auto& adj : host.out( attach_point ) )
            {
                outgoing.emplace_back( adj.target, adj.data );
            }

            [[maybe_unused]]
            auto rr     = host.rid( attach_point );

            auto remap  = detail::splice_sub_into( host, sub );
            auto roots  = detail::sub_roots( sub );
            auto leaves = detail::sub_leaves( sub );

            for( const auto& [src, data] : incoming )
            {
                for( const auto& root : roots )
                {
                    [[maybe_unused]]
                    auto r = host.tie( src, remap.at( root ), data );
                }
            }
            for( const auto& [dst, data] : outgoing )
            {
                for( const auto& leaf : leaves )
                {
                    [[maybe_unused]]
                    auto r = host.tie( remap.at( leaf ), dst, data );
                }
            }
            return host;
        }
    }

}    // namespace walk
