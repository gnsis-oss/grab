#pragma once
// ┌───────────────────────────────────────────────────────────────────────┐
// │  tag/content_id.hpp — tag::ContentId content-addressed 32-byte blob  │
// └───────────────────────────────────────────────────────────────────────┘
//
// ContentId is a typed wrapper around 32 bytes — the shape of a
// SHA-256 output. It is distinct from tag::Uuid (128-bit random/timed
// identifier) because it addresses bytes, not entities.
//
// Used by yoke (task content-addressing) and data-lineage systems
// like Dagster's DataVersion, Ray's object-ref content addressing.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>

namespace tag
{

    // ── ContentId ──────────────────────────────────────────────────────────────

    // Field name follows the ticket's public API. NOLINT silences the
    // project-wide MemberSuffix rule for this documented vocabulary struct.

    struct ContentId
    {
            static constexpr std::size_t      byte_count = 32;

            // NOLINTNEXTLINE(readability-identifier-naming)
            std::array<std::byte, byte_count> bytes{};

            [[nodiscard]]
            constexpr bool
            operator==( const ContentId& ) const noexcept = default;

            [[nodiscard]]
            constexpr auto
            operator<=>( const ContentId& ) const noexcept = default;

            [[nodiscard]]
            constexpr bool
            nil() const noexcept
            {
                return std::ranges::all_of( bytes,
                                            []( std::byte b )
                                            {
                                                return b == std::byte{ 0 };
                                            } );
            }
    };

    // ── Hex formatting (debug) ─────────────────────────────────────────────────

    namespace detail
    {

        inline constexpr std::array<char, 16> hex_digits = {
            '0',
            '1',
            '2',
            '3',
            '4',
            '5',
            '6',
            '7',
            '8',
            '9',
            'a',
            'b',
            'c',
            'd',
            'e',
            'f'
        };

        inline constexpr unsigned hex_base       = 10;
        inline constexpr uint8_t  low_nibble     = 0X0FU;
        inline constexpr unsigned high_shift     = 4;
        inline constexpr unsigned chars_per_byte = 2;

        [[nodiscard]]
        inline int
        content_id_nibble( char c ) noexcept
        {
            if( c >= '0' && c <= '9' )
            {
                return c - '0';
            }
            if( c >= 'a' && c <= 'f' )
            {
                return ( c - 'a' ) + static_cast<int>( hex_base );
            }
            if( c >= 'A' && c <= 'F' )
            {
                return ( c - 'A' ) + static_cast<int>( hex_base );
            }
            return -1;
        }

    }    // namespace detail

    [[nodiscard]]
    inline std::string
    to_hex( const ContentId& c )
    {
        std::string s;
        s.resize( ContentId::byte_count * detail::chars_per_byte );
        for( std::size_t i = 0; i < ContentId::byte_count; ++i )
        {
            const auto v   = std::to_integer<uint8_t>( c.bytes.at( i ) );
            const auto v_u = static_cast<unsigned>( v );
            s.at( ( i * detail::chars_per_byte ) + 0 ) =
                detail::hex_digits.at( static_cast<std::size_t>(
                    ( v_u >> detail::high_shift ) & detail::low_nibble
                ) );
            s.at( ( i * detail::chars_per_byte ) + 1 ) = detail::hex_digits.at(
                static_cast<std::size_t>( v_u & detail::low_nibble )
            );
        }
        return s;
    }

    // Parse a 64-char hex string into a ContentId. Returns false if the
    // input is the wrong length or contains non-hex characters.
    [[nodiscard]]
    inline bool
    try_from_hex( std::string_view sv,
                  ContentId&       out_id ) noexcept
    {
        if( sv.size() != ContentId::byte_count * detail::chars_per_byte )
        {
            return false;
        }
        for( std::size_t i = 0; i < ContentId::byte_count; ++i )
        {
            const int hi =
                detail::content_id_nibble( sv[( i * detail::chars_per_byte ) + 0] );
            const int lo =
                detail::content_id_nibble( sv[( i * detail::chars_per_byte ) + 1] );
            if( hi < 0 || lo < 0 )
            {
                return false;
            }
            const auto hi_u = static_cast<unsigned>( hi );
            const auto lo_u = static_cast<unsigned>( lo );
            out_id.bytes.at( i ) =
                static_cast<std::byte>( ( hi_u << detail::high_shift ) | lo_u );
        }
        return true;
    }

}    // namespace tag

// ── std::hash ──────────────────────────────────────────────────────────────

template<>
struct std::hash<tag::ContentId>
{
        [[nodiscard]]
        std::size_t
        operator()( const tag::ContentId& c ) const noexcept
        {
            // SHA-256 output is already uniform — load the first 8 bytes.
            std::size_t v{ 0 };
            std::memcpy( &v, c.bytes.data(), sizeof( v ) );
            return v;
        }
};
