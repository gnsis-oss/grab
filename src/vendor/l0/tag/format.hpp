#pragma once
// ┌───────────────────────────────────────────────────────────────────────┐
// │  tag/format.h — to_string, from_string, std::formatter, operator<<   │
// └───────────────────────────────────────────────────────────────────────┘
//
// to_string:   Always big-endian hex. For native integer types (N<=64),
//              byteswaps to big-endian first on little-endian systems.
//
// from_string: Permissive parser. Strips whitespace, braces {}, parens (),
//              0x prefix, urn:uuid: prefix, and dashes. Returns out::Put.
//
// std::formatter<tag::Id<N>>:
//   {}   or {:x} — hex without dashes
//   {:d}          — hex with dashes (8-4-4-4-12, 128-bit only)
//   other spec    — throws std::format_error
//
// operator<< for ostream: hex without dashes.

#include <bit>
#include <cstdint>
#include <cstring>
#include <format>
#include <ostream>
#include <out/put.hpp>
#include <string>
#include <string_view>
#include <tag/tag.hpp>

// NOLINTBEGIN — UUID hex codec; byte-level layout per RFC 9562 §4.
namespace tag
{

    // ── detail: hex encoding ─────────────────────────────────────────────────────

    namespace detail
    {

        inline constexpr char
        hex_nibble( unsigned v ) noexcept
        {
            return ( v < 10 ) ? static_cast<char>( '0' + v )
                              : static_cast<char>( 'a' + v - 10 );
        }

        // Encode a single byte as two hex chars into dest[0] and dest[1].
        inline void
        encode_byte( char*   dest,
                     uint8_t b ) noexcept
        {
            dest[0] = hex_nibble( ( b >> 4 ) & 0X0FU );
            dest[1] = hex_nibble( b & 0X0FU );
        }

        // Decode a hex nibble. Returns -1 on invalid char.
        inline int32_t
        decode_nibble( char c ) noexcept
        {
            if( c >= '0' && c <= '9' )
            {
                return c - '0';
            }
            if( c >= 'a' && c <= 'f' )
            {
                return c - 'a' + 10;
            }
            if( c >= 'A' && c <= 'F' )
            {
                return c - 'A' + 10;
            }
            return -1;
        }

    }    // namespace detail

    // ── to_string ────────────────────────────────────────────────────────────────

    template<unsigned N>
    [[nodiscard]]
    std::string
    to_string( const Id<N>& t )
    {
        constexpr unsigned byte_count = Id<N>::byte_count;
        std::string        result( byte_count * 2, '\0' );

        if constexpr( detail::is_native_int<N> )
        {
            // Read native int, byteswap to big-endian on LE systems.
            detail::storage_t<N> val{};
            std::memcpy( &val, t.bytes(), sizeof( val ) );
            if constexpr( std::endian::native == std::endian::little )
            {
                val = std::byteswap( val );
            }
            // Encode the (now big-endian) bytes.
            const auto* bytes = reinterpret_cast<const uint8_t*>( &val );
            for( unsigned i = 0; i < byte_count; ++i )
            {
                detail::encode_byte( result.data() + i * 2, bytes[i] );
            }
        }
        else
        {
            // Array storage: bytes are already in canonical order.
            const uint8_t* bytes = t.bytes();
            for( unsigned i = 0; i < byte_count; ++i )
            {
                detail::encode_byte( result.data() + i * 2, bytes[i] );
            }
        }
        return result;
    }

    // ── from_string ──────────────────────────────────────────────────────────────

    template<unsigned N>
    [[nodiscard]]
    out::Put<Id<N>,
             out::Error>
    from_string( std::string_view sv )
    {
        constexpr unsigned byte_count = Id<N>::byte_count;

        // Strip leading/trailing whitespace.
        while( !sv.empty() && ( sv.front() ==
                                ' ' ||
                                sv.front() ==
                                '\t' ||
                                sv.front() ==
                                '\n' ||
                                sv.front() == '\r' ) )
        {
            sv.remove_prefix( 1 );
        }
        while( !sv.empty() && ( sv.back() ==
                                ' ' ||
                                sv.back() ==
                                '\t' ||
                                sv.back() ==
                                '\n' ||
                                sv.back() == '\r' ) )
        {
            sv.remove_suffix( 1 );
        }

        // Strip urn:uuid: prefix (case-insensitive comparison not needed per spec).
        if( sv.starts_with( "urn:uuid:" ) )
        {
            sv.remove_prefix( 9 );
        }

        // Strip surrounding braces {} or parentheses () (only when both present).
        if( sv.size() >= 2 )
        {
            if( ( sv.front() == '{' && sv.back() == '}' ) ||
                ( sv.front() == '(' && sv.back() == ')' ) )
            {
                sv.remove_prefix( 1 );
                sv.remove_suffix( 1 );
            }
        }

        // Strip 0x prefix.
        if( sv.size() >= 2 && sv[0] == '0' && ( sv[1] == 'x' || sv[1] == 'X' ) )
        {
            sv.remove_prefix( 2 );
        }

        // Build a clean hex string by removing dashes.
        std::string hex;
        hex.reserve( sv.size() );
        for( char c : sv )
        {
            if( c != '-' )
            {
                hex.push_back( c );
            }
        }

        // Validate length: must be exactly byte_count * 2 hex digits.
        if( hex.size() != byte_count * 2 )
        {
            return out::Error::wrong;
        }

        // Parse hex bytes (big-endian: first hex chars = most significant byte).
        uint8_t buf[byte_count];
        for( unsigned i = 0; i < byte_count; ++i )
        {
            int32_t hi = detail::decode_nibble( hex[i * 2] );
            int32_t lo = detail::decode_nibble( hex[i * 2 + 1] );
            if( hi < 0 || lo < 0 )
            {
                return out::Error::wrong;
            }
            buf[i] = static_cast<uint8_t>( ( hi << 4 ) | lo );
        }

        if constexpr( detail::is_native_int<N> )
        {
            // The bytes are big-endian; byteswap to host order on LE systems.
            detail::storage_t<N> val{};
            std::memcpy( &val, buf, sizeof( val ) );
            if constexpr( std::endian::native == std::endian::little )
            {
                val = std::byteswap( val );
            }
            return Id<N>{ val };
        }
        else
        {
            typename Id<N>::storage_type arr{};
            std::memcpy( arr.data(), buf, byte_count );
            return Id<N>{ arr };
        }
    }

    // ── operator<< ───────────────────────────────────────────────────────────────

    template<unsigned N>
    std::ostream&
    operator<<( std::ostream& os,
                const Id<N>&  t )
    {
        return os << to_string( t );
    }

}    // namespace tag

// ── std::formatter<tag::Id<N>> ───────────────────────────────────────────────

template<unsigned N>
struct std::formatter<tag::Id<N>>
{
        // Format spec: empty or 'x' → plain hex; 'd' → dashed hex (128-bit only).
        char spec_ = 'x';

        constexpr auto
        parse( std::format_parse_context& ctx )
        {
            auto it = ctx.begin();
            if( it == ctx.end() || *it == '}' )
            {
                spec_ = 'x';
                return it;
            }
            char c = *it;
            if( c == 'x' || c == 'd' )
            {
                spec_ = c;
                ++it;
            }
            else
            {
                // Unknown spec — we cannot throw during constexpr parse on all
                // implementations, so store a sentinel and throw in format().
                spec_ = '?';
                ++it;
                // Skip to closing '}'.
                while( it != ctx.end() && *it != '}' )
                {
                    ++it;
                }
            }
            return it;
        }

        auto
        format( const tag::Id<N>&    t,
                std::format_context& ctx ) const
        {
            if( spec_ == '?' )
            {
                throw std::format_error( "tag::Id: unknown format spec" );
            }
            if( spec_ == 'd' )
            {
                static_assert(
                    N == 128 || true,
                    "tag::Id: {:d} format requires Id<128>; check is at runtime because "
                    "N is a template parameter of the formatter, not the format call"
                );
                if constexpr( N != 128 )
                {
                    // N is known at compile time but std::format("{:d}", some_non_128)
                    // can only be caught at runtime within the formatter protocol.
                    throw std::format_error( "tag::Id: {:d} requires Id<128>" );
                }
                else
                {
                    // 8-4-4-4-12 hex-dash format.
                    auto        plain = tag::to_string( t );
                    // plain is 32 hex chars; insert dashes at positions 8,12,16,20.
                    std::string s;
                    s.reserve( 36 );
                    s.append( plain, 0, 8 );
                    s.push_back( '-' );
                    s.append( plain, 8, 4 );
                    s.push_back( '-' );
                    s.append( plain, 12, 4 );
                    s.push_back( '-' );
                    s.append( plain, 16, 4 );
                    s.push_back( '-' );
                    s.append( plain, 20, 12 );
                    return std::format_to( ctx.out(), "{}", s );
                }
            }
            // Default: plain hex.
            return std::format_to( ctx.out(), "{}", tag::to_string( t ) );
        }
};

// NOLINTEND
