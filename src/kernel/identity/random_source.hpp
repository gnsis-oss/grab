#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

// The random half of a UUID.
//
// IdFactory writes the first eight bytes itself — a 48-bit millisecond
// timestamp, the version nibble and a within-millisecond counter — so the only
// thing left to source is the trailing random block. Seeded once from
// std::random_device and drawn from a Mersenne twister after that: identifiers
// have to be unique and unguessable-enough to not collide, not
// cryptographically unpredictable.

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>

namespace grab::detail
{

    class RandomSource final
    {
        public:

            RandomSource() :
                engine_{ std::random_device{}() }
            {
            }

            [[nodiscard]]
            std::uint64_t
            next() noexcept
            {
                return engine_();
            }

            // Fills `bytes` with random octets, little-endian within each drawn
            // word — the byte order does not matter, only that every octet is
            // random and the fill is exact for any Count.
            template<std::size_t Count>
            [[nodiscard]]
            std::array<std::uint8_t,
                       Count>
            next_bytes() noexcept
            {
                constexpr std::size_t           bitsPerByte  = 8U;
                constexpr std::size_t           bytesPerWord = sizeof( std::uint64_t );
                constexpr std::uint64_t         byteMask     = 0XFFU;

                std::array<std::uint8_t, Count> bytes{};
                std::size_t                     written = 0U;
                while( written < Count )
                {
                    auto word = engine_();
                    for( std::size_t index = 0U; index < bytesPerWord && written < Count;
                         ++index )
                    {
                        bytes.at( written ) =
                            static_cast<std::uint8_t>( word & byteMask );
                        word >>= bitsPerByte;
                        ++written;
                    }
                }
                return bytes;
            }

        private:

            std::mt19937_64 engine_;
    };

}    // namespace grab::detail
