#pragma once
// ┌──────────────────────────────────────────────────────────────────────┐
// │  walk/parts.h -- connected-component partition                      │
// └──────────────────────────────────────────────────────────────────────┘
//
// Partitions a graph into connected components using union-find.
// On AnyWay graphs: standard connected components.
// On OneWay graphs: weakly-connected components (edges treated as undirected).
//
// Visitor hooks (optional):
//   on_find(knot)                       -- knot first seen
//   on_parts_found(span<const Knot>)    -- component finalized

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
#include <walk/detail/union_find.hpp>
#include <walk/strategy.hpp>
#include <web/concept.hpp>
#include <web/knot.hpp>
#include <web/web.hpp>

namespace walk
{

    struct Partition
    {
            std::vector<std::uint32_t>          component_of;
            std::vector<std::vector<web::Knot>> components;
            std::uint32_t                       count = 0;
    };

    namespace detail
    {

        template<typename Policy,
                 typename E,
                 typename Vis>
        [[nodiscard]]
        Partition
        parts_impl( const web::Web<Policy,
                                   E>& g,
                    Vis&               visitor )
        {
            auto knots_vec = g.knots();
            if( knots_vec.empty() )
            {
                return Partition{};
            }

            // Map each knot to a dense index [0, V)
            std::unordered_map<web::Knot, int> idx;
            idx.reserve( knots_vec.size() );
            for( std::size_t i = 0; i < knots_vec.size(); ++i )
            {
                idx.emplace( knots_vec[i], static_cast<int>( i ) );
                if constexpr( requires { visitor.on_find( knots_vec[i] ); } )
                {
                    visitor.on_find( knots_vec[i] );
                }
            }

            Kin<ByRank, Halve> kin( knots_vec.size() );

            for( const auto& from : knots_vec )
            {
                auto out_span = g.out( from );
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
                    kin.bond( idx.at( from ), idx.at( to ) );
                }

                if constexpr( std::is_same_v<Policy, web::OneWay> )
                {
                    auto in_span = g.in( from );
                    for( const auto& adj : in_span )
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
                        kin.bond( idx.at( from ), idx.at( to ) );
                    }
                }
            }

            // Build Partition from kin roots.
            std::unordered_map<int, std::uint32_t> root_to_component;
            Partition                              out;
            out.component_of.resize( knots_vec.size() );

            for( std::size_t i = 0; i < knots_vec.size(); ++i )
            {
                const int root      = kin.find( static_cast<int>( i ) );
                auto [it, inserted] = root_to_component.try_emplace( root, out.count );
                if( inserted )
                {
                    out.components.emplace_back();
                    ++out.count;
                }
                std::uint32_t cid   = it->second;
                out.component_of[i] = cid;
                out.components[cid].push_back( knots_vec[i] );
            }

            if constexpr( requires {
                              visitor.on_parts_found( std::span<const web::Knot>{} );
                          } )
            {
                for( const auto& comp : out.components )
                {
                    visitor.on_parts_found( std::span<const web::Knot>{ comp } );
                }
            }

            return out;
        }

        struct PartsNoVisitor
        {
        };

    }    // namespace detail

    template<typename Policy,
             typename E>
    [[nodiscard]]
    Partition
    parts( const web::Web<Policy,
                          E>& g )
    {
        detail::PartsNoVisitor nv;
        return detail::parts_impl( g, nv );
    }

    template<typename Policy,
             typename E,
             typename Vis>
    [[nodiscard]]
    Partition
    parts( const web::Web<Policy,
                          E>& g,
           Vis&               visitor )
    {
        return detail::parts_impl( g, visitor );
    }

}    // namespace walk
