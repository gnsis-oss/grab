#include "grab/image.hpp"
#include "grab/result.hpp"
#include "image/compare.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstddef>
#include <cstdint>
#include <vector>
// clang-format on

namespace
{

    constexpr std::uint32_t kImageWidth         = 4U;
    constexpr std::uint32_t kImageHeight        = 4U;
    constexpr std::uint32_t kMismatchedWidth    = 5U;
    constexpr std::uint32_t kRgbaBytes          = 4U;
    constexpr std::uint32_t kRgbaStride         = kImageWidth * kRgbaBytes;
    constexpr std::uint32_t kChangedBlockX      = 1U;
    constexpr std::uint32_t kChangedBlockY      = 1U;
    constexpr std::uint32_t kChangedBlockWidth  = 2U;
    constexpr std::uint32_t kChangedBlockHeight = 2U;
    constexpr std::uint32_t kChangedBlockEndX   = kChangedBlockX + kChangedBlockWidth;
    constexpr std::uint32_t kChangedBlockEndY   = kChangedBlockY + kChangedBlockHeight;
    constexpr std::int32_t  kOriginX            = 0;
    constexpr std::int32_t  kOriginY            = 0;
    constexpr std::uint32_t kSingleChangedX     = 2U;
    constexpr std::uint32_t kSingleChangedY     = 3U;
    constexpr std::uint64_t kChangedBlockPixels = 4U;
    constexpr std::uint64_t kNoDiffPixels       = 0U;
    constexpr std::uint64_t kTotalPixels = static_cast<std::uint64_t>( kImageWidth ) *
                                           static_cast<std::uint64_t>( kImageHeight );
    constexpr std::uint64_t kChangedBlockMatches = kTotalPixels - kChangedBlockPixels;
    constexpr double        kFullMatchRatio      = 1.0;
    constexpr std::uint8_t  kBaseRed             = 40U;
    constexpr std::uint8_t  kBaseGreen           = 80U;
    constexpr std::uint8_t  kBaseBlue            = 120U;
    constexpr std::uint8_t  kBaseAlpha           = 160U;
    constexpr std::uint8_t  kChangedRed          = 190U;
    constexpr std::uint8_t  kChangedGreen        = 30U;
    constexpr std::uint8_t  kChangedBlue         = 210U;
    constexpr std::uint8_t  kChangedAlpha        = 250U;
    constexpr std::uint8_t  kToleranceDelta      = 3U;
    constexpr std::uint8_t  kDiffRed             = 0XFFU;
    constexpr std::uint8_t  kDiffGreen           = 0U;
    constexpr std::uint8_t  kDiffBlue            = 0U;
    constexpr std::uint8_t  kDiffAlpha           = 0XFFU;
    constexpr std::size_t   kRedOffset           = 0U;
    constexpr std::size_t   kGreenOffset         = 1U;
    constexpr std::size_t   kBlueOffset          = 2U;
    constexpr std::size_t   kAlphaOffset         = 3U;
    constexpr auto          kInvalidArgument     = grab::ErrorCode::invalid_argument;

    [[nodiscard]]
    std::byte
    byte_from( std::uint8_t value ) noexcept
    {
        return static_cast<std::byte>( value );
    }

    [[nodiscard]]
    std::uint8_t
    byte_value( std::byte value ) noexcept
    {
        return static_cast<std::uint8_t>( value );
    }

    [[nodiscard]]
    std::size_t
    pixel_offset( const grab::Image& image,
                  std::uint32_t      x,
                  std::uint32_t      y ) noexcept
    {
        return ( static_cast<std::size_t>( y ) *
                 static_cast<std::size_t>( image.stride ) ) +
               ( static_cast<std::size_t>( x ) *
                 static_cast<std::size_t>( kRgbaBytes ) );
    }

    void
    set_pixel( grab::Image&  image,
               std::uint32_t x,
               std::uint32_t y,
               std::uint8_t  red,
               std::uint8_t  green,
               std::uint8_t  blue,
               std::uint8_t  alpha )
    {
        const auto offset                        = pixel_offset( image, x, y );
        image.pixels.at( offset + kRedOffset )   = byte_from( red );
        image.pixels.at( offset + kGreenOffset ) = byte_from( green );
        image.pixels.at( offset + kBlueOffset )  = byte_from( blue );
        image.pixels.at( offset + kAlphaOffset ) = byte_from( alpha );
    }

    [[nodiscard]]
    grab::Image
    make_rgba_image( std::uint32_t width,
                     std::uint32_t height )
    {
        const auto  stride = width * kRgbaBytes;
        grab::Image image{
            .width  = width,
            .height = height,
            .stride = stride,
            .format = grab::PixelFormat::rgba,
            .pixels = std::vector<std::byte>( static_cast<std::size_t>( stride ) *
                                              static_cast<std::size_t>( height ) ),
        };

        for( std::uint32_t y = 0U; y < height; ++y )
        {
            for( std::uint32_t x = 0U; x < width; ++x )
            {
                set_pixel( image, x, y, kBaseRed, kBaseGreen, kBaseBlue, kBaseAlpha );
            }
        }

        return image;
    }

    [[nodiscard]]
    double
    ratio( std::uint64_t matching_pixels ) noexcept
    {
        return static_cast<double>( matching_pixels ) /
               static_cast<double>( kTotalPixels );
    }

    void
    change_block( grab::Image& image )
    {
        for( std::uint32_t y = kChangedBlockY; y < kChangedBlockEndY; ++y )
        {
            for( std::uint32_t x = kChangedBlockX; x < kChangedBlockEndX; ++x )
            {
                set_pixel( image,
                           x,
                           y,
                           kChangedRed,
                           kChangedGreen,
                           kChangedBlue,
                           kChangedAlpha );
            }
        }
    }

    void
    add_tolerance_delta_to_all_pixels( grab::Image& image )
    {
        for( auto& pixel_byte : image.pixels )
        {
            const auto value =
                static_cast<std::uint8_t>( byte_value( pixel_byte ) + kToleranceDelta );
            pixel_byte = byte_from( value );
        }
    }

    void
    expect_rect_eq( const grab::image::Rect& actual,
                    const grab::image::Rect& expected )
    {
        EXPECT_EQ( actual.x, expected.x );
        EXPECT_EQ( actual.y, expected.y );
        EXPECT_EQ( actual.width, expected.width );
        EXPECT_EQ( actual.height, expected.height );
    }

}    // namespace

TEST( Compare,
      IdenticalImagesMatchFully )
{
    const auto first  = make_rgba_image( kImageWidth, kImageHeight );
    const auto second = make_rgba_image( kImageWidth, kImageHeight );

    const auto result = grab::image::compare( first, second );

    ASSERT_TRUE( result.has_value() );
    EXPECT_DOUBLE_EQ( result->match_ratio, kFullMatchRatio );
    EXPECT_EQ( result->diff_pixels, kNoDiffPixels );
    EXPECT_FALSE( result->bounding_box.has_value() );
}

TEST( Compare,
      ChangedRegionIsCounted )
{
    const auto first  = make_rgba_image( kImageWidth, kImageHeight );
    auto       second = make_rgba_image( kImageWidth, kImageHeight );
    change_block( second );

    const auto result = grab::image::compare( first, second );

    ASSERT_TRUE( result.has_value() );
    EXPECT_DOUBLE_EQ( result->match_ratio, ratio( kChangedBlockMatches ) );
    EXPECT_EQ( result->diff_pixels, kChangedBlockPixels );
    ASSERT_TRUE( result->bounding_box.has_value() );
    expect_rect_eq( *result->bounding_box,
                    grab::image::Rect{
                        .x      = static_cast<std::int32_t>( kChangedBlockX ),
                        .y      = static_cast<std::int32_t>( kChangedBlockY ),
                        .width  = kChangedBlockWidth,
                        .height = kChangedBlockHeight,
                    } );
}

TEST( Compare,
      ToleranceAbsorbsSmallDeltas )
{
    const auto first  = make_rgba_image( kImageWidth, kImageHeight );
    auto       second = make_rgba_image( kImageWidth, kImageHeight );
    add_tolerance_delta_to_all_pixels( second );

    const auto tolerant = grab::image::compare(
        first,
        second,
        grab::image::CompareOptions{ .per_channel_tolerance = kToleranceDelta }
    );
    ASSERT_TRUE( tolerant.has_value() );
    EXPECT_EQ( tolerant->diff_pixels, kNoDiffPixels );
    EXPECT_FALSE( tolerant->bounding_box.has_value() );

    const auto exact = grab::image::compare( first, second );
    ASSERT_TRUE( exact.has_value() );
    EXPECT_EQ( exact->diff_pixels, kTotalPixels );
    ASSERT_TRUE( exact->bounding_box.has_value() );
    expect_rect_eq( *exact->bounding_box,
                    grab::image::Rect{
                        .x      = kOriginX,
                        .y      = kOriginY,
                        .width  = kImageWidth,
                        .height = kImageHeight,
                    } );
}

TEST( Compare,
      MismatchedDimsRejected )
{
    const auto first  = make_rgba_image( kImageWidth, kImageHeight );
    const auto second = make_rgba_image( kMismatchedWidth, kImageHeight );

    const auto result = grab::image::compare( first, second );

    ASSERT_FALSE( result.has_value() );
    EXPECT_EQ( result.error().code, kInvalidArgument );
}

TEST( Diff,
      DiffImageHighlightsChanges )
{
    const auto first  = make_rgba_image( kImageWidth, kImageHeight );
    auto       second = make_rgba_image( kImageWidth, kImageHeight );
    set_pixel( second,
               kSingleChangedX,
               kSingleChangedY,
               kChangedRed,
               kChangedGreen,
               kChangedBlue,
               kChangedAlpha );

    const auto result = grab::image::diff_image( first, second );

    ASSERT_TRUE( result.has_value() );
    EXPECT_EQ( result->width, kImageWidth );
    EXPECT_EQ( result->height, kImageHeight );
    EXPECT_EQ( result->stride, kRgbaStride );
    EXPECT_EQ( result->format, grab::PixelFormat::rgba );

    const auto offset = pixel_offset( *result, kSingleChangedX, kSingleChangedY );
    EXPECT_EQ( result->pixels.at( offset + kRedOffset ), byte_from( kDiffRed ) );
    EXPECT_EQ( result->pixels.at( offset + kGreenOffset ), byte_from( kDiffGreen ) );
    EXPECT_EQ( result->pixels.at( offset + kBlueOffset ), byte_from( kDiffBlue ) );
    EXPECT_EQ( result->pixels.at( offset + kAlphaOffset ), byte_from( kDiffAlpha ) );
}
