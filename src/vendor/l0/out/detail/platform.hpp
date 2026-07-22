#pragma once
// ┌──────────────────────────────────────────────────────────────────────┐
// │  out/detail/platform.h -- Platform detection and SIMD dispatch      │
// └──────────────────────────────────────────────────────────────────────┘
//
// Detects: compiler (GCC/Clang/MSVC), architecture (x86_64/ARM64),
// SIMD capability (SSE4.2, AVX2, NEON), and provides portable wrappers.
//
// Usage: #include <out/detail/platform.hpp>
//        if constexpr (out::detail::has_sse42) { ... }

#include <cstdint>
#include <cstring>

namespace out::detail
{

// ── Compiler Detection ───────────────────────────────────────
#if defined( __GNUC__ ) && !defined( __clang__ )
    inline constexpr bool is_gcc   = true;
    inline constexpr bool is_clang = false;
    inline constexpr bool is_msvc  = false;
#elif defined( __clang__ )
    inline constexpr bool is_gcc   = false;
    inline constexpr bool is_clang = true;
    inline constexpr bool is_msvc  = false;
#elif defined( _MSC_VER )
    inline constexpr bool is_gcc   = false;
    inline constexpr bool is_clang = false;
    inline constexpr bool is_msvc  = true;
#else
    inline constexpr bool is_gcc   = false;
    inline constexpr bool is_clang = false;
    inline constexpr bool is_msvc  = false;
#endif

// ── Architecture Detection ───────────────────────────────────
#if defined( __x86_64__ ) || defined( _M_X64 )
    inline constexpr bool is_x86_64  = true;
    inline constexpr bool is_aarch64 = false;
#elif defined( __aarch64__ ) || defined( _M_ARM64 )
    inline constexpr bool is_x86_64  = false;
    inline constexpr bool is_aarch64 = true;
#else
    inline constexpr bool is_x86_64  = false;
    inline constexpr bool is_aarch64 = false;
#endif

// ── SIMD Capability Detection ────────────────────────────────
#if defined( __SSE4_2__ )
    inline constexpr bool has_sse42 = true;
#else
    inline constexpr bool has_sse42 = false;
#endif

#if defined( __AVX2__ )
    inline constexpr bool has_avx2 = true;
#else
    inline constexpr bool has_avx2 = false;
#endif

#if defined( __AVX512F__ )
    inline constexpr bool has_avx512 = true;
#else
    inline constexpr bool has_avx512 = false;
#endif

#if defined( __ARM_NEON ) || defined( __ARM_NEON__ )
    inline constexpr bool has_neon = true;
#else
    inline constexpr bool has_neon = false;
#endif

    // ── Cache Line Size ──────────────────────────────────────────
    // Hardcoded to 64: stable across compilers and avoids GCC -Winterference-size.
    // Correct for x86_64 and aarch64. Revisit only for exotic targets.
    inline constexpr std::size_t cache_line = 64;

// ── Compiler Hints ───────────────────────────────────────────
#if defined( __GNUC__ ) || defined( __clang__ )
    #define SEED_LIKELY( x )        __builtin_expect( !!( x ), 1 )
    #define SEED_UNLIKELY( x )      __builtin_expect( !!( x ), 0 )
    #define SEED_ALWAYS_INLINE      __attribute__( ( always_inline ) ) inline
    #define SEED_NOINLINE           __attribute__( ( noinline ) )
    #define SEED_PREFETCH( addr )   __builtin_prefetch( addr, 0, 1 )
    #define SEED_PREFETCH_W( addr ) __builtin_prefetch( addr, 1, 1 )
#elif defined( _MSC_VER )
    #define SEED_LIKELY( x )   ( x )
    #define SEED_UNLIKELY( x ) ( x )
    #define SEED_ALWAYS_INLINE __forceinline
    #define SEED_NOINLINE      __declspec( noinline )
    #define SEED_PREFETCH( addr )                                          \
        _mm_prefetch( reinterpret_cast<const char*>( addr ), _MM_HINT_T1 )
    #define SEED_PREFETCH_W( addr )                                        \
        _mm_prefetch( reinterpret_cast<const char*>( addr ), _MM_HINT_T1 )
#else
    #define SEED_LIKELY( x )   ( x )
    #define SEED_UNLIKELY( x ) ( x )
    #define SEED_ALWAYS_INLINE inline
    #define SEED_NOINLINE
    #define SEED_PREFETCH( addr )   ( ( void )0 )
    #define SEED_PREFETCH_W( addr ) ( ( void )0 )
#endif

    // ── Portable SIMD Wrappers ───────────────────────────────────

    // Fast memchr -- uses SIMD internally on glibc/musl/MSVC CRT
    // This is the single most impactful "free SIMD" optimization
    SEED_ALWAYS_INLINE const uint8_t*
    fast_find_byte( const uint8_t* p,
                    std::size_t    len,
                    uint8_t        byte )
    {
        return static_cast<const uint8_t*>( std::memchr( p, byte, len ) );
    }

    // Fast 8-byte comparison -- uses native uint64_t load
    SEED_ALWAYS_INLINE bool
    fast_eq8( const void* a,
              const void* b )
    {
        uint64_t va, vb;
        std::memcpy( &va, a, 8 );
        std::memcpy( &vb, b, 8 );
        return va == vb;
    }

    // Fast N-byte comparison -- 8 bytes at a time
    SEED_ALWAYS_INLINE bool
    fast_eq( const void* a,
             const void* b,
             std::size_t n )
    {
        auto pa = static_cast<const uint8_t*>( a );
        auto pb = static_cast<const uint8_t*>( b );
        while( n >= 8 )
        {
            if( !fast_eq8( pa, pb ) )
            {
                return false;
            }
            pa += 8;
            pb += 8;
            n  -= 8;
        }
        while( n-- )
        {
            if( *pa++ != *pb++ )
            {
                return false;
            }
        }
        return true;
    }

}    // namespace out::detail

// ── Architecture-Specific Headers ────────────────────────────
// These are included conditionally and provide platform-specific
// SIMD functions in out::detail::x86 or out::detail::arm namespaces.

#if defined( __SSE4_2__ ) && ( defined( __GNUC__ ) || defined( __clang__ ) )
    #include <out/detail/x86/sse42.hpp>
#endif

#if defined( __AVX2__ ) && ( defined( __GNUC__ ) || defined( __clang__ ) )
    #include <out/detail/x86/avx2.hpp>
#endif

#if ( defined( __ARM_NEON ) || defined( __ARM_NEON__ ) ) && \
    ( defined( __GNUC__ ) || defined( __clang__ ) )
    #include <out/detail/arm/neon.hpp>
#endif
