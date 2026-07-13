#pragma once
// ┌──────────────────────────────────────────────────────────────────────┐
// │  walk/split.h -- edge-cut fragmentation                             │
// └──────────────────────────────────────────────────────────────────────┘
//
// Non-destructively removes a set of edges from a graph and returns the
// resulting connected fragments. Edges not present in the graph are silently
// ignored (no error).

#include <cstdint>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
#include <walk/parts.hpp>
#include <web/knot.hpp>
#include <web/web.hpp>

namespace walk
{

    template<typename P, typename E>
    struct Fragments
    {
            std::vector<web::Web<P, E>> pieces;
    };

    template<typename P,
             typename E>
    [[nodiscard]]
    Fragments<P,
              E>
    split( const web::Web<P,
                          E>&                    g,
           std::span<const std::pair<web::Knot,
                                     web::Knot>> cut_edges )
    {
        // Copy the graph, then cut. web::Web::cut returns Error if edge absent;
        // we ignore that — "silently ignored" is the contract.
        web::Web<P, E> working;
        for( const auto& kn : g.knots() )
        {
            [[maybe_unused]]
            auto r = working.add( kn );
        }
        for( const auto& from : g.knots() )
        {
            auto span = g.out( from );
            for( const auto& adj : span )
            {
                if constexpr( std::is_void_v<E> )
                {
                    [[maybe_unused]]
                    auto r = working.tie( from, adj );
                }
                else
                {
                    [[maybe_unused]]
                    auto r = working.tie( from, adj.target, adj.data );
                }
            }
        }
        for( const auto& c : cut_edges )
        {
            [[maybe_unused]]
            auto r = working.cut( c.first, c.second );
        }

        // Partition the residual graph.
        Partition       part = parts( working );

        Fragments<P, E> out;
        out.pieces.reserve( part.count );

        // Build a knot -> component_id index from part.components
        std::unordered_map<web::Knot, std::uint32_t> cid_of;
        for( std::uint32_t c = 0; c < part.count; ++c )
        {
            for( const auto& kn : part.components[c] )
            {
                cid_of.emplace( kn, c );
            }
        }

        for( std::uint32_t c = 0; c < part.count; ++c )
        {
            web::Web<P, E> piece;
            for( const auto& kn : part.components[c] )
            {
                [[maybe_unused]]
                auto r = piece.add( kn );
            }
            for( const auto& from : part.components[c] )
            {
                auto span = working.out( from );
                for( const auto& adj : span )
                {
                    web::Knot to{};
                    if constexpr( std::is_void_v<E> )
                    {
                        to = adj;
                    }
                    else
                    {
                        to = adj.target;
                    }
                    if( cid_of.at( to ) == c )
                    {
                        if constexpr( std::is_void_v<E> )
                        {
                            [[maybe_unused]]
                            auto rt = piece.tie( from, to );
                        }
                        else
                        {
                            [[maybe_unused]]
                            auto rt = piece.tie( from, to, adj.data );
                        }
                    }
                }
            }
            out.pieces.push_back( std::move( piece ) );
        }

        return out;
    }

}    // namespace walk
