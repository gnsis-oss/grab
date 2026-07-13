#pragma once
// ┌──────────────────────────────────────────────────────────────────────┐
// │  heap/trait.h -- heap::Trait<T>, concepts for heapable items        │
// └──────────────────────────────────────────────────────────────────────┘
//
// Every item type stored in a Heap must specialize heap::Trait<T> with:
//   static tag::Id<64> id(const T&)   -- unique identity for index lookup
//   static auto key(const T&)         -- comparable value for ordering
//
// Concepts HasId, HasKey, and Heapable validate trait conformance at
// compile time.

#include <concepts>
#include <tag/tag.hpp>
#include <type_traits>

namespace heap
{

    // ── Primary template -- user must specialize ────────────────────────────

    template<typename T>
    struct Trait;

    // ── Concepts ────────────────────────────────────────────────────────────

    template<typename T>
    concept HasId = requires( const T& t ) {
        {
            Trait<T>::id( t )
        } -> std::convertible_to<tag::Id<64>>;
    };

    template<typename T>
    concept HasKey = requires( const T& t ) {
        {
            Trait<T>::key( t )
        } -> std::totally_ordered;
    };

    template<typename T>
    concept Heapable = HasId<T> && HasKey<T>;

}    // namespace heap
