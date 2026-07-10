#include "grab/image.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstddef>
#include <cstdint>
#include <vector>
// clang-format on

namespace
{

    constexpr std::uint32_t bgraBytes     = 4U;
    constexpr std::uint32_t rgbaBytes     = 4U;
    constexpr std::uint32_t rgbBytes      = 3U;
    constexpr std::uint32_t bgrBytes      = 3U;
    constexpr std::uint32_t grayBytes     = 1U;
    constexpr std::uint32_t width         = 3U;
    constexpr std::uint32_t height        = 2U;
    constexpr std::uint32_t paddingBytes  = 2U;
    constexpr std::uint32_t stride        = ( width * rgbBytes ) + paddingBytes;
    constexpr std::uint32_t outOfRangeRow = height;
    constexpr std::size_t   pixelSeed     = 17U;
    constexpr std::size_t   byteModulus   = 256U;
    constexpr std::size_t   expectedBytes =
        static_cast<std::size_t>( stride ) * static_cast<std::size_t>( height );

    [[nodiscard]]
    std::byte
    test_byte( std::size_t index ) noexcept
    {
        return static_cast<std::byte>(
            static_cast<unsigned char>( ( index + pixelSeed ) % byteModulus )
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
    static_assert( grab::bytes_per_pixel( grab::PixelFormat::Bgra ) == bgraBytes );
    static_assert( grab::bytes_per_pixel( grab::PixelFormat::Rgba ) == rgbaBytes );
    static_assert( grab::bytes_per_pixel( grab::PixelFormat::Rgb ) == rgbBytes );
    static_assert( grab::bytes_per_pixel( grab::PixelFormat::Bgr ) == bgrBytes );
    static_assert( grab::bytes_per_pixel( grab::PixelFormat::Gray ) == grayBytes );

    EXPECT_EQ( grab::bytes_per_pixel( grab::PixelFormat::Bgra ), bgraBytes );
    EXPECT_EQ( grab::bytes_per_pixel( grab::PixelFormat::Rgba ), rgbaBytes );
    EXPECT_EQ( grab::bytes_per_pixel( grab::PixelFormat::Rgb ), rgbBytes );
    EXPECT_EQ( grab::bytes_per_pixel( grab::PixelFormat::Bgr ), bgrBytes );
    EXPECT_EQ( grab::bytes_per_pixel( grab::PixelFormat::Gray ), grayBytes );
}

TEST( Image,
      EmptyFollowsDimensions )
{
    const grab::Image empty_width{
        .width  = 0U,
        .height = height,
        .stride = stride,
        .format = grab::PixelFormat::Rgb,
        .pixels = pixels( expectedBytes ),
    };
    EXPECT_TRUE( empty_width.empty() );

    const grab::Image empty_height{
        .width  = width,
        .height = 0U,
        .stride = stride,
        .format = grab::PixelFormat::Rgb,
        .pixels = pixels( expectedBytes ),
    };
    EXPECT_TRUE( empty_height.empty() );

    const grab::Image non_empty{
        .width  = width,
        .height = height,
        .stride = stride,
        .format = grab::PixelFormat::Rgb,
        .pixels = pixels( expectedBytes ),
    };
    EXPECT_FALSE( non_empty.empty() );
}

TEST( Image,
      RowUsesStrideAndBounds )
{
    const auto        backing_pixels = pixels( expectedBytes );
    const grab::Image image{
        .width  = width,
        .height = height,
        .stride = stride,
        .format = grab::PixelFormat::Rgb,
        .pixels = backing_pixels,
    };

    const auto row_zero = image.row( 0U );
    ASSERT_EQ( row_zero.size(), stride );
    EXPECT_EQ( row_zero.front(), backing_pixels.front() );
    EXPECT_EQ( row_zero.back(), backing_pixels.at( stride - 1U ) );

    const auto row_one = image.row( 1U );
    ASSERT_EQ( row_one.size(), stride );
    EXPECT_EQ( row_one.front(), backing_pixels.at( stride ) );

    EXPECT_TRUE( image.row( outOfRangeRow ).empty() );

    const grab::Image truncated{
        .width  = width,
        .height = height,
        .stride = stride,
        .format = grab::PixelFormat::Rgb,
        .pixels = pixels( stride ),
    };
    EXPECT_TRUE( truncated.row( 1U ).empty() );
}
