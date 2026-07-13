#pragma once
// ┌──────────────────────────────────────────────────────────────────────┐
// │  web/knot.h -- web::Knot, type-safe vertex wrapping tag::Id<64>     │
// └──────────────────────────────────────────────────────────────────────┘
//
// Knot is a vertex identity. Two knots with the same tag::Id<64> are the
// same vertex. Supports equality, ordering, and std::hash for use in
// both ordered and unordered containers.

#include <compare>
#include <cstdint>
#include <functional>
#include <tag/tag.hpp>

namespace web
{

    class Knot
    {
            tag::Id<64> id_;

        public:

            Knot() = default;

            constexpr explicit Knot( uint64_t raw ) noexcept :
                id_( raw )
            {
            }

            constexpr explicit Knot( tag::Id<64> id ) noexcept :
                id_( id )
            {
            }

            [[nodiscard]]
            tag::Id<64>
            id() const noexcept
            {
                return id_;
            }

            [[nodiscard]]
            bool
            nil() const noexcept
            {
                return id_.nil();
            }

            [[nodiscard]]
            bool
            operator==( const Knot& ) const noexcept = default;

            [[nodiscard]]
            auto
            operator<=>( const Knot& other ) const noexcept
            {
                return id_ <=> other.id_;
            }
    };

}    // namespace web

// ── std::hash ───────────────────────────────────────────────────────────

namespace std
{

    template<>
    struct hash<web::Knot>
    {
            std::size_t
            operator()( const web::Knot& k ) const noexcept
            {
                return std::hash<tag::Id<64>>{}( k.id() );
            }
    };

}    // namespace std
