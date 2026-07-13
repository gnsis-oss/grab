#pragma once
// ┌────────────────────────────────────────────────┐
// │  tag/tag.h — tag::Id<N> compile-time identifier │
// └────────────────────────────────────────────────┘
//
// tag::Id<N> is an N-bit identifier with:
//  - Compact storage (native int for 8/16/32/64, array otherwise)
//  - alignas(16) for Id<128>
//  - Zero-initialized by default (nil state)
//  - Equality, ordering, and std::hash

#include <algorithm>
#include <array>
#include <bit>
#include <compare>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string_view>
#include <type_traits>

namespace tag
{

    // ── Storage type selection ────────────────────────────────────────────────────

    namespace detail
    {

        template<unsigned N>
        struct storage
        {
                using type = std::array<uint8_t, ( N + 7 ) / 8>;
        };

        template<>
        struct storage<8>
        {
                using type = uint8_t;
        };

        template<>
        struct storage<16>
        {
                using type = uint16_t;
        };

        template<>
        struct storage<32>
        {
                using type = uint32_t;
        };

        template<>
        struct storage<64>
        {
                using type = uint64_t;
        };

        template<>
        struct storage<128>
        {
                using type = std::array<uint8_t, 16>;
        };

        template<unsigned N>
        using storage_t = typename storage<N>::type;

        // Is the storage a native integer type (not array)?
        template<unsigned N>
        inline constexpr bool is_native_int = N == 8 || N == 16 || N == 32 || N == 64;

    }    // namespace detail

    // ── rng_source concept ────────────────────────────────────────────────────────

    template<typename T>
    concept rng_source =
        std::invocable<T&> && std::unsigned_integral<std::invoke_result_t<T&>>;

    // ── Id<N> primary template ────────────────────────────────────────────────────

    template<unsigned N>
    class alignas( N == 128 ? 16 : alignof( detail::storage_t<N> ) ) Id
    {
            static_assert( N > 0,
                           "tag::Id<N> requires N > 0" );

        public:

            using storage_type = detail::storage_t<N>;

        private:

            // Grant hash access to private data_
            friend struct std::hash<Id<N>>;

            storage_type data_{};    // zero-initialized by default member initializer

        public:

            static constexpr unsigned bits       = N;
            static constexpr unsigned byte_count = ( N + 7 ) / 8;

            // ── Construction ─────────────────────────────────────────────────────────

            Id() = default;

            constexpr explicit Id( storage_type raw ) noexcept :
                data_{ raw }
            {
            }

            // ── Accessors ─────────────────────────────────────────────────────────────

            [[nodiscard]]
            const uint8_t*
            bytes() const noexcept
            {
                if constexpr( detail::is_native_int<N> )
                {
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                    return reinterpret_cast<const uint8_t*>( &data_ );
                }
                else
                {
                    return data_.data();
                }
            }

            [[nodiscard]]
            bool
            nil() const noexcept
            {
                if constexpr( detail::is_native_int<N> )
                {
                    return data_ == storage_type{ 0 };
                }
                else
                {
                    return std::ranges::all_of( data_,
                                                []( auto b )
                                                {
                                                    return b == 0;
                                                } );
                }
            }

            // ── Equality ─────────────────────────────────────────────────────────────

            [[nodiscard]]
            bool
            operator==( const Id& other ) const noexcept
            {
                if constexpr( N == 128 )
                {
                    // Branchless XOR-OR: load two uint64_t halves via memcpy
                    uint64_t a0 = 0;
                    uint64_t a1 = 0;
                    uint64_t b0 = 0;
                    uint64_t b1 = 0;
                    std::memcpy( &a0, data_.data(), 8 );
                    std::memcpy( &a1, data_.data() + 8, 8 );
                    std::memcpy( &b0, other.data_.data(), 8 );
                    std::memcpy( &b1, other.data_.data() + 8, 8 );
                    return ( ( a0 ^ b0 ) | ( a1 ^ b1 ) ) == 0;
                }
                else
                {
                    return data_ == other.data_;
                }
            }

            [[nodiscard]]
            bool
            operator!=( const Id& other ) const noexcept
            {
                // NOLINTNEXTLINE(readability-redundant-parentheses)
                return !( *this == other );
            }

            // ── Ordering ─────────────────────────────────────────────────────────────

            [[nodiscard]]
            auto
            operator<=>( const Id& other ) const noexcept
            {
                return data_ <=> other.data_;
            }
    };

    // Verify size constraints at instantiation time
    static_assert( sizeof( Id<8> ) == 1 );
    static_assert( sizeof( Id<16> ) == 2 );
    static_assert( sizeof( Id<32> ) == 4 );
    static_assert( sizeof( Id<64> ) == 8 );
    static_assert( sizeof( Id<128> ) == 16 );

    // Verify alignment of Id<128>
    static_assert( alignof( Id<128> ) == 16 );

}    // namespace tag

// ── std::hash specializations ─────────────────────────────────────────────────

namespace std
{

    template<unsigned N>
    struct hash<tag::Id<N>>
    {
            std::size_t
            operator()( const tag::Id<N>& v ) const noexcept
            {
                if constexpr( N == 8 )
                {
                    return std::hash<uint8_t>{}( v.data_ );
                }
                else if constexpr( N == 16 )
                {
                    return std::hash<uint16_t>{}( v.data_ );
                }
                else if constexpr( N == 32 )
                {
                    return std::hash<uint32_t>{}( v.data_ );
                }
                else if constexpr( N == 64 )
                {
                    return std::hash<uint64_t>{}( v.data_ );
                }
                else if constexpr( N == 128 )
                {
                    // Direct-load first 8 bytes as uint64_t
                    uint64_t lo = 0;
                    std::memcpy( &lo, v.data_.data(), 8 );
                    return std::hash<uint64_t>{}( lo );
                }
                else
                {
                    // Generic: hash the bytes as a string_view
                    return std::hash<std::string_view>{}( std::string_view{
                        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                        reinterpret_cast<const char*>( v.bytes() ),
                        tag::Id<N>::byte_count
                    } );
                }
            }
    };

}    // namespace std
