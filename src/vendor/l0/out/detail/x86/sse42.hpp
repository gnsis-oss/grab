#pragma once
#include <cstddef>
#include <cstdint>
#include <immintrin.h>

namespace out::detail::x86
{

    // Scan up to 16 bytes for any byte in a set of delimiters
    // Returns index of first match (0-15) or 16 if none found
    SEED_ALWAYS_INLINE int
    find_delimiters_16( const uint8_t* data,
                        std::size_t    len,
                        const __m128i& delims,
                        int            delim_count )
    {
        __m128i chunk = _mm_loadu_si128( reinterpret_cast<const __m128i*>( data ) );
        return _mm_cmpestri( delims,
                             delim_count,
                             chunk,
                             static_cast<int>( len < 16 ? len : 16 ),
                             _SIDD_UBYTE_OPS |
                                 _SIDD_CMP_EQUAL_ANY |
                                 _SIDD_LEAST_SIGNIFICANT );
    }

    // Check if 16 bytes contain any byte from a set
    SEED_ALWAYS_INLINE bool
    has_delimiter_16( const uint8_t* data,
                      std::size_t    len,
                      const __m128i& delims,
                      int            delim_count )
    {
        __m128i chunk = _mm_loadu_si128( reinterpret_cast<const __m128i*>( data ) );
        return _mm_cmpestrc( delims,
                             delim_count,
                             chunk,
                             static_cast<int>( len < 16 ? len : 16 ),
                             _SIDD_UBYTE_OPS | _SIDD_CMP_EQUAL_ANY );
    }

}    // namespace out::detail::x86
