#pragma once
#include <cstdint>
#include <immintrin.h>

namespace out::detail::x86
{

    // Compare 32 bytes at once for equality with a single byte
    // Returns bitmask of matching positions
    SEED_ALWAYS_INLINE uint32_t
    find_byte_32( const uint8_t* data,
                  uint8_t        byte )
    {
        __m256i needle = _mm256_set1_epi8( static_cast<char>( byte ) );
        __m256i haystack =
            _mm256_loadu_si256( reinterpret_cast<const __m256i*>( data ) );
        __m256i cmp = _mm256_cmpeq_epi8( haystack, needle );
        return static_cast<uint32_t>( _mm256_movemask_epi8( cmp ) );
    }

    // Compare 32 bytes for equality between two buffers
    SEED_ALWAYS_INLINE bool
    eq_32( const void* a,
           const void* b )
    {
        __m256i va  = _mm256_loadu_si256( static_cast<const __m256i*>( a ) );
        __m256i vb  = _mm256_loadu_si256( static_cast<const __m256i*>( b ) );
        __m256i cmp = _mm256_cmpeq_epi8( va, vb );
        return _mm256_movemask_epi8( cmp ) == -1;    // all bytes equal
    }

}    // namespace out::detail::x86
