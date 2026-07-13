#pragma once
// ┌────────────────────────────────────────────────────────────────────┐
// │  tag/gen.h — generator functions for tag::Id<N>                   │
// └────────────────────────────────────────────────────────────────────┘
//
// All generators live here:
//
//   random<N>(rng)                  — RFC 9562 v4 for N==128, raw random otherwise
//   named(ns, name)                 — RFC 9562 v5 (SHA-1 namespace + name) [Task 6]
//   blend(a, b)                     — RFC 9562 v5 (SHA-1 of two ids)      [Task 6]
//   timed(rng)                      — RFC 9562 v7 (48-bit Unix ms + random)[Task 7]
//   make(span<const uint8_t, 16>)   — RFC 9562 v8 (user payload)           [Task 7]

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <log/writer.hpp>
#include <span>
#include <string_view>
#include <tag/detail/sha1.hpp>
#include <tag/tag.hpp>

// NOLINTBEGIN — UUID v4/v5 bit-packing per RFC 9562 §5.4/§5.5.
namespace tag
{

    // ── detail helpers ────────────────────────────────────────────────────────────

    namespace detail
    {

        // Fill an N-bit byte array from an rng_source.
        // Adapts to the actual word size returned by the RNG.
        template<unsigned   N,
                 rng_source Rng>
        [[nodiscard]]
        std::array<uint8_t,
                   ( N + 7 ) / 8>
        fill_random_bytes( Rng& rng )
        {
            constexpr unsigned byte_count             = ( N + 7 ) / 8;
            using word_type                           = std::invoke_result_t<Rng&>;
            constexpr unsigned              word_size = sizeof( word_type );

            std::array<uint8_t, byte_count> buf{};
            uint32_t                        written = 0;
            while( written < byte_count )
            {
                auto     word = rng();
                uint32_t take = ( byte_count - written < word_size )
                                  ? ( byte_count - written )
                                  : word_size;
                std::memcpy( buf.data() + written, &word, take );
                written += take;
            }
            return buf;
        }

        // Stamp RFC 9562 v4 version/variant bits into a 16-byte array.
        // Byte 6: version nibble 0x4?
        // Byte 8: variant bits  10xxxxxx
        inline void
        stamp_v4( uint8_t* p ) noexcept
        {
            p[6] = static_cast<uint8_t>( ( p[6] & 0X0FU ) | 0X40U );
            p[8] = static_cast<uint8_t>( ( p[8] & 0X3FU ) | 0X80U );
        }

        // Stamp RFC 9562 v5 version/variant bits into a 16-byte array.
        // Byte 6: version nibble 0x5?
        // Byte 8: variant bits  10xxxxxx
        inline void
        stamp_v5( uint8_t* p ) noexcept
        {
            p[6] = static_cast<uint8_t>( ( p[6] & 0X0FU ) | 0X50U );
            p[8] = static_cast<uint8_t>( ( p[8] & 0X3FU ) | 0X80U );
        }

        // Stamp RFC 9562 v7 version/variant bits into a 16-byte array.
        // Byte 6: version nibble 0x7?
        // Byte 8: variant bits  10xxxxxx
        inline void
        stamp_v7( uint8_t* p ) noexcept
        {
            p[6] = static_cast<uint8_t>( ( p[6] & 0X0FU ) | 0X70U );
            p[8] = static_cast<uint8_t>( ( p[8] & 0X3FU ) | 0X80U );
        }

        // Stamp RFC 9562 v8 version/variant bits into a 16-byte array.
        // Byte 6: version nibble 0x8?
        // Byte 8: variant bits  10xxxxxx
        inline void
        stamp_v8( uint8_t* p ) noexcept
        {
            p[6] = static_cast<uint8_t>( ( p[6] & 0X0FU ) | 0X80U );
            p[8] = static_cast<uint8_t>( ( p[8] & 0X3FU ) | 0X80U );
        }

    }    // namespace detail

    // ── random<N>(rng) ────────────────────────────────────────────────────────────
    //
    // Generates a random Id<N>.
    // For N == 128: stamps RFC 9562 v4 version and variant bits.
    // For N <= 64 (native int): returns raw random bits with no stamping.

    template<unsigned   N = 128,
             rng_source Rng>
    [[nodiscard]]
    Id<N>
    random( Rng& rng )
    {
        logger::trace( logger::tag( "tag.gen" ), "random() generating v4 ID" );
        if constexpr( detail::is_native_int<N> )
        {
            // Native int storage: fill bytes then reconstruct
            auto                         buf = detail::fill_random_bytes<N>( rng );
            typename Id<N>::storage_type val{};
            std::memcpy( &val, buf.data(), sizeof( val ) );
            return Id<N>{ val };
        }
        else
        {
            // Array storage (including N==128)
            auto buf = detail::fill_random_bytes<N>( rng );
            if constexpr( N == 128 )
            {
                detail::stamp_v4( buf.data() );
            }
            typename Id<N>::storage_type arr{};
            std::memcpy( arr.data(), buf.data(), buf.size() );
            return Id<N>{ arr };
        }
    }

    // ── named(ns, name) ───────────────────────────────────────────────────────────
    //
    // RFC 9562 v5: SHA-1(namespace_bytes || name_bytes), first 16 bytes, v5 stamped.

    [[nodiscard]]
    inline Id<128>
    named( const Id<128>&   ns,
           std::string_view name )
    {
        logger::trace( logger::tag( "tag.gen" ), "named() generating v5 ID" );
        detail::sha1 h;
        h.update( ns.bytes(), 16 );
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        h.update( reinterpret_cast<const uint8_t*>( name.data() ), name.size() );
        auto                    digest = h.digest();

        std::array<uint8_t, 16> buf{};
        std::memcpy( buf.data(), digest.data(), 16 );
        detail::stamp_v5( buf.data() );
        return Id<128>{ buf };
    }

    // ── blend(a, b) ─────────────────────────────────────────────────────────────
    //
    // RFC 9562 v5: SHA-1(a_bytes || b_bytes), first 16 bytes, v5 stamped.

    [[nodiscard]]
    inline Id<128>
    blend( const Id<128>& a,
           const Id<128>& b )
    {
        logger::trace( logger::tag( "tag.gen" ), "blend() combining IDs" );
        detail::sha1 h;
        h.update( a.bytes(), 16 );
        h.update( b.bytes(), 16 );
        auto                    digest = h.digest();

        std::array<uint8_t, 16> buf{};
        std::memcpy( buf.data(), digest.data(), 16 );
        detail::stamp_v5( buf.data() );
        return Id<128>{ buf };
    }

    // ── timed(rng) ────────────────────────────────────────────────────────────────
    //
    // RFC 9562 v7: 48-bit Unix timestamp (ms) in big-endian bytes[0..5],
    // followed by random bits, then v7 version/variant stamped.

    template<rng_source Rng>
    [[nodiscard]]
    Id<128>
    timed( Rng& rng )
    {
        logger::trace( logger::tag( "tag.gen" ), "timed() generating v7 ID" );
        // Fill with random bytes first
        auto buf = detail::fill_random_bytes<128>( rng );

        // Overwrite first 6 bytes with 48-bit Unix ms timestamp (big-endian)
        using clock  = std::chrono::system_clock;
        auto     now = clock::now().time_since_epoch();
        uint64_t ms  = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>( now ).count()
        );

        buf[0] = static_cast<uint8_t>( ( ms >> 40 ) & 0XFFU );
        buf[1] = static_cast<uint8_t>( ( ms >> 32 ) & 0XFFU );
        buf[2] = static_cast<uint8_t>( ( ms >> 24 ) & 0XFFU );
        buf[3] = static_cast<uint8_t>( ( ms >> 16 ) & 0XFFU );
        buf[4] = static_cast<uint8_t>( ( ms >> 8 ) & 0XFFU );
        buf[5] = static_cast<uint8_t>( ( ms >> 0 ) & 0XFFU );

        detail::stamp_v7( buf.data() );

        std::array<uint8_t, 16> arr{};
        std::memcpy( arr.data(), buf.data(), 16 );
        return Id<128>{ arr };
    }

    // ── make(span) ──────────────────────────────────────────────────────────────
    //
    // RFC 9562 v8: copies caller-supplied 16 bytes, stamps v8 version/variant bits.

    [[nodiscard]]
    inline Id<128>
    make( std::span<const uint8_t,
                    16> payload )
    {
        logger::trace( logger::tag( "tag.gen" ), "make() generating v8 ID" );
        std::array<uint8_t, 16> buf{};
        std::memcpy( buf.data(), payload.data(), 16 );
        detail::stamp_v8( buf.data() );
        return Id<128>{ buf };
    }

}    // namespace tag

// NOLINTEND
