#pragma once
#include <arm_neon.h>
#include <cstdint>

namespace out::detail::arm
{

    // Compare 16 bytes for equality with a single byte
    // Returns bitmask-like result
    SEED_ALWAYS_INLINE uint64_t
    find_byte_16( const uint8_t* data,
                  uint8_t        byte )
    {
        uint8x16_t needle   = vdupq_n_u8( byte );
        uint8x16_t haystack = vld1q_u8( data );
        uint8x16_t cmp      = vceqq_u8( haystack, needle );
        // Reduce to scalar -- check if any lane matched
        return vmaxvq_u8( cmp );
    }

    // Compare 16 bytes for equality between two buffers
    SEED_ALWAYS_INLINE bool
    eq_16( const void* a,
           const void* b )
    {
        uint8x16_t va  = vld1q_u8( static_cast<const uint8_t*>( a ) );
        uint8x16_t vb  = vld1q_u8( static_cast<const uint8_t*>( b ) );
        uint8x16_t cmp = vceqq_u8( va, vb );
        return vminvq_u8( cmp ) == 0XFF;    // all bytes equal
    }

}    // namespace out::detail::arm
