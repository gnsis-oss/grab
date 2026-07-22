#pragma once
// ┌─────────────────────────────────────────────────────────────────────────┐
// │  tag/detail/sha1.h — vendored SHA-1 per FIPS 180-4                     │
// └─────────────────────────────────────────────────────────────────────────┘
//
// Self-contained, no dependencies beyond <cstdint> and <array>.
// Used by tag::named() and tag::blend() to generate v5 UUIDs.
//
// API:
//   sha1 h;
//   h.update(ptr, len);            // can be called multiple times
//   auto d = h.digest();           // returns std::array<uint8_t, 20>
//                                  // (sha1 object is consumed/reset after this)

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

// NOLINTBEGIN — vendored SHA-1 per FIPS 180-4; algorithm style preserved.
namespace tag::detail
{

    class sha1
    {
        public:

            sha1() noexcept
            {
                reset();
            }

            // Append bytes to the running hash.
            void
            update( const uint8_t* data,
                    std::size_t    len ) noexcept
            {
                while( len > 0 )
                {
                    block_[block_len_++] = *data++;
                    --len;
                    bit_count_ += 8;
                    if( block_len_ == 64 )
                    {
                        compress();
                        block_len_ = 0;
                    }
                }
            }

            // Finalise and return the 20-byte digest.
            // The sha1 object is reset to initial state after this call.
            [[nodiscard]]
            std::array<uint8_t,
                       20>
            digest() noexcept
            {
                // Padding: append 0x80, zero bytes, then 64-bit big-endian bit count
                uint64_t total_bits  = bit_count_;

                block_[block_len_++] = 0X80U;
                if( block_len_ > 56 )
                {
                    // Not enough room for length — finish this block, start another
                    while( block_len_ < 64 )
                    {
                        block_[block_len_++] = 0X00U;
                    }
                    compress();
                    block_len_ = 0;
                }
                while( block_len_ < 56 )
                {
                    block_[block_len_++] = 0X00U;
                }

                // Append 64-bit big-endian message length in bits
                for( int32_t i = 7; i >= 0; --i )
                {
                    block_[block_len_++] =
                        static_cast<uint8_t>( ( total_bits >> ( i * 8 ) ) & 0XFFU );
                }
                compress();

                // Extract digest (big-endian uint32 per SHA-1 spec)
                std::array<uint8_t, 20> out{};
                for( int32_t i = 0; i < 5; ++i )
                {
                    out[static_cast<std::size_t>( i * 4 + 0 )] =
                        static_cast<uint8_t>( ( h_[i] >> 24 ) & 0XFFU );
                    out[static_cast<std::size_t>( i * 4 + 1 )] =
                        static_cast<uint8_t>( ( h_[i] >> 16 ) & 0XFFU );
                    out[static_cast<std::size_t>( i * 4 + 2 )] =
                        static_cast<uint8_t>( ( h_[i] >> 8 ) & 0XFFU );
                    out[static_cast<std::size_t>( i * 4 + 3 )] =
                        static_cast<uint8_t>( ( h_[i] >> 0 ) & 0XFFU );
                }

                reset();
                return out;
            }

        private:

            void
            reset() noexcept
            {
                h_[0] = 0X67'45'23'01U;
                h_[1] = 0XEF'CD'AB'89U;
                h_[2] = 0X98'BA'DC'FEU;
                h_[3] = 0X10'32'54'76U;
                h_[4] = 0XC3'D2'E1'F0U;
                std::memset( block_, 0, sizeof( block_ ) );
                block_len_ = 0;
                bit_count_ = 0;
            }

            static constexpr uint32_t
            rotl( uint32_t x,
                  unsigned n ) noexcept
            {
                return ( x << n ) | ( x >> ( 32U - n ) );
            }

            void
            compress() noexcept
            {
                uint32_t w[80];

                // Prepare message schedule
                for( int32_t i = 0; i < 16; ++i )
                {
                    w[i]  = static_cast<uint32_t>( block_[i * 4 + 0] ) << 24;
                    w[i] |= static_cast<uint32_t>( block_[i * 4 + 1] ) << 16;
                    w[i] |= static_cast<uint32_t>( block_[i * 4 + 2] ) << 8;
                    w[i] |= static_cast<uint32_t>( block_[i * 4 + 3] );
                }
                for( int32_t i = 16; i < 80; ++i )
                {
                    w[i] = rotl( w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1 );
                }

                uint32_t a = h_[0];
                uint32_t b = h_[1];
                uint32_t c = h_[2];
                uint32_t d = h_[3];
                uint32_t e = h_[4];

                for( int32_t i = 0; i < 80; ++i )
                {
                    uint32_t f, k;
                    if( i < 20 )
                    {
                        f = ( b & c ) | ( ~b & d );
                        k = 0X5A'82'79'99U;
                    }
                    else if( i < 40 )
                    {
                        f = b ^ c ^ d;
                        k = 0X6E'D9'EB'A1U;
                    }
                    else if( i < 60 )
                    {
                        f = ( b & c ) | ( b & d ) | ( c & d );
                        k = 0X8F'1B'BC'DCU;
                    }
                    else
                    {
                        f = b ^ c ^ d;
                        k = 0XCA'62'C1'D6U;
                    }

                    uint32_t temp = rotl( a, 5 ) + f + e + k + w[i];
                    e             = d;
                    d             = c;
                    c             = rotl( b, 30 );
                    b             = a;
                    a             = temp;
                }

                h_[0] += a;
                h_[1] += b;
                h_[2] += c;
                h_[3] += d;
                h_[4] += e;
            }

            uint32_t h_[5];
            uint8_t  block_[64];
            uint8_t  block_len_;
            uint64_t bit_count_;
    };

}    // namespace tag::detail

// NOLINTEND
