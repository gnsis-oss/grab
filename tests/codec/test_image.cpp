#include "grab/image.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstddef>
#include <cstdint>
#include <vector>
// clang-format on

namespace
{

    constexpr std::uint32_t kBgraBytes     = 4U;
    constexpr std::uint32_t kRgbaBytes     = 4U;
    constexpr std::uint32_t kRgbBytes      = 3U;
    constexpr std::uint32_t kBgrBytes      = 3U;
    constexpr std::uint32_t kGrayBytes     = 1U;
    constexpr std::uint32_t kWidth         = 3U;
    constexpr std::uint32_t kHeight        = 2U;
    constexpr std::uint32_t kPaddingBytes  = 2U;
    constexpr std::uint32_t kStride        = ( kWidth * kRgbBytes ) + kPaddingBytes;
    constexpr std::uint32_t kOutOfRangeRow = kHeight;
    constexpr std::size_t   kPixelSeed     = 17U;
    constexpr std::size_t   kByteModulus   = 256U;
    constexpr std::size_t   kExpectedBytes =
        static_cast<std::size_t>( kStride ) * static_cast<std::size_t>( kHeight );

    [[nodiscard]]
    std::byte
    test_byte( std::size_t index ) noexcept
    {
        return static_cast<std::byte>(
            static_cast<unsigned char>( ( index + kPixelSeed ) % kByteModulus )
        );
    }

    [[nodiscard]]
    std::vector<std::byte>
    pixels( std::size_t size )
    {
        std::vector<std::byte> result;
        result.reserve( size );
        for( std::size_t index = 0U; index < size; ++index )
        {
            result.push_back( test_byte( index ) );
        }
        return result;
    }

}    // namespace

TEST( Image,
      BytesPerPixelIsStable )
{
    static_assert( grab::bytes_per_pixel( grab::PixelFormat::bgra ) == kBgraBytes );
    static_assert( grab::bytes_per_pixel( grab::PixelFormat::rgba ) == kRgbaBytes );
    static_assert( grab::bytes_per_pixel( grab::PixelFormat::rgb ) == kRgbBytes );
    static_assert( grab::bytes_per_pixel( grab::PixelFormat::bgr ) == kBgrBytes );
    static_assert( grab::bytes_per_pixel( grab::PixelFormat::gray ) == kGrayBytes );

    EXPECT_EQ( grab::bytes_per_pixel( grab::PixelFormat::bgra ), kBgraBytes );
    EXPECT_EQ( grab::bytes_per_pixel( grab::PixelFormat::rgba ), kRgbaBytes );
    EXPECT_EQ( grab::bytes_per_pixel( grab::PixelFormat::rgb ), kRgbBytes );
    EXPECT_EQ( grab::bytes_per_pixel( grab::PixelFormat::bgr ), kBgrBytes );
    EXPECT_EQ( grab::bytes_per_pixel( grab::PixelFormat::gray ), kGrayBytes );
}

TEST( Image,
      EmptyFollowsDimensions )
{
    const grab::Image empty_width{
        .width  = 0U,
        .height = kHeight,
        .stride = kStride,
        .format = grab::PixelFormat::rgb,
        .pixels = pixels( kExpectedBytes ),
    };
    EXPECT_TRUE( empty_width.empty() );

    const grab::Image empty_height{
        .width  = kWidth,
        .height = 0U,
        .stride = kStride,
        .format = grab::PixelFormat::rgb,
        .pixels = pixels( kExpectedBytes ),
    };
    EXPECT_TRUE( empty_height.empty() );

    const grab::Image non_empty{
        .width  = kWidth,
        .height = kHeight,
        .stride = kStride,
        .format = grab::PixelFormat::rgb,
        .pixels = pixels( kExpectedBytes ),
    };
    EXPECT_FALSE( non_empty.empty() );
}

TEST( Image,
      RowUsesStrideAndBounds )
{
    const auto        backing_pixels = pixels( kExpectedBytes );
    const grab::Image image{
        .width  = kWidth,
        .height = kHeight,
        .stride = kStride,
        .format = grab::PixelFormat::rgb,
        .pixels = backing_pixels,
    };

    const auto row_zero = image.row( 0U );
    ASSERT_EQ( row_zero.size(), kStride );
    EXPECT_EQ( row_zero.front(), backing_pixels.front() );
    EXPECT_EQ( row_zero.back(), backing_pixels.at( kStride - 1U ) );

    const auto row_one = image.row( 1U );
    ASSERT_EQ( row_one.size(), kStride );
    EXPECT_EQ( row_one.front(), backing_pixels.at( kStride ) );

    EXPECT_TRUE( image.row( kOutOfRangeRow ).empty() );

    const grab::Image truncated{
        .width  = kWidth,
        .height = kHeight,
        .stride = kStride,
        .format = grab::PixelFormat::rgb,
        .pixels = pixels( kStride ),
    };
    EXPECT_TRUE( truncated.row( 1U ).empty() );
}
