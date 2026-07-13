#pragma once
// ┌────────────────────────────────────────────────────────────────────────┐
// │  tag/idx.h — tag::Idx<Tag, Rep> typed dense index                     │
// └────────────────────────────────────────────────────────────────────────┘
//
// Idx<Tag, Rep> — typed dense index for array-indexed collections.
// Tag is an empty struct for type uniqueness.
// Rep is the underlying integer type (default: int).
//
// Unlike Id<N> (globally unique UUID), Idx is a lightweight wrapper
// around a small integer for O(1) array indexing. Prevents mixing
// different index domains (node index vs edge index vs point index).

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace tag
{

    template<typename Tag, typename Rep = int32_t>
    struct Idx
    {
            Rep v{};

            constexpr Idx() = default;

            constexpr explicit Idx( Rep value ) :
                v( value )
            {
            }

            [[nodiscard]]
            constexpr Rep
            raw() const
            {
                return v;
            }

            [[nodiscard]]
            constexpr bool
            operator==( const Idx& other ) const = default;
            [[nodiscard]]
            constexpr auto
            operator<=>( const Idx& other ) const = default;

            // Increment/decrement for iteration
            constexpr Idx&
            operator++()
            {
                ++v;
                return *this;
            }

            constexpr Idx
            operator++( int )
            {
                auto tmp = *this;
                ++v;
                return tmp;
            }

            constexpr Idx&
            operator--()
            {
                --v;
                return *this;
            }

            constexpr Idx
            operator--( int )
            {
                auto tmp = *this;
                --v;
                return tmp;
            }
    };

}    // namespace tag

// std::hash specialization for use in unordered containers
template<typename Tag, typename Rep>
struct std::hash<tag::Idx<Tag, Rep>>
{
        std::size_t
        operator()( tag::Idx<Tag,
                             Rep> idx ) const noexcept
        {
            return std::hash<Rep>{}( idx.v );
        }
};
