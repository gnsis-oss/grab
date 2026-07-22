#pragma once
// ┌───────────────────────────────────────────────────────────────┐
// │  tag/rng.h — random number sources for tag generators         │
// └───────────────────────────────────────────────────────────────┘
//
// Provides two RNG sources that satisfy the tag::rng_source concept:
//
//   FastRng   — wraps std::mt19937_64, seeded from std::random_device
//   SafeRng — wraps std::random_device directly (combines two 32-bit words)
//
// Both types produce uint64_t values via operator()().

#include <cstdint>
#include <random>
#include <tag/tag.hpp>

namespace tag
{

    // ── FastRng ──────────────────────────────────────────────────────────────────
    //
    // Mersenne Twister 64-bit PRNG, seeded once from std::random_device.
    // Fast, statistically high-quality, NOT cryptographically secure.

    class FastRng
    {
        public:

            FastRng() :
                engine_( std::random_device{}() )
            {
            }

            uint64_t
            operator()() noexcept
            {
                return engine_();
            }

        private:

            std::mt19937_64 engine_;
    };

    static_assert( rng_source<FastRng> );

    // ── SafeRng ────────────────────────────────────────────────────────────────
    //
    // Wraps std::random_device, which on Linux uses /dev/urandom.
    // Combines two 32-bit words into one 64-bit value.
    // Slower than FastRng but relies on OS entropy directly.

    class SafeRng
    {
        public:

            SafeRng() = default;

            uint64_t
            operator()()
            {
                const uint64_t hi = device_();
                const uint64_t lo = device_();
                return ( hi << 32U ) | lo;
            }

        private:

            std::random_device device_;
    };

    static_assert( rng_source<SafeRng> );

}    // namespace tag
