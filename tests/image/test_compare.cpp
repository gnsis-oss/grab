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

    constexpr std::uint32_t imageWidth         = 4U;
    constexpr std::uint32_t imageHeight        = 4U;
    constexpr std::uint32_t mismatchedWidth    = 5U;
    constexpr std::uint32_t rgbaBytes          = 4U;
    constexpr std::uint32_t rgbaStride         = imageWidth * rgbaBytes;
    constexpr std::uint32_t changedBlockX      = 1U;
    constexpr std::uint32_t changedBlockY      = 1U;
    constexpr std::uint32_t changedBlockWidth  = 2U;
    constexpr std::uint32_t changedBlockHeight = 2U;
    constexpr std::uint32_t changedBlockEndX   = changedBlockX + changedBlockWidth;
    constexpr std::uint32_t changedBlockEndY   = changedBlockY + changedBlockHeight;
    constexpr std::int32_t  originX            = 0;
    constexpr std::int32_t  originY            = 0;
    constexpr std::uint32_t singleChangedX     = 2U;
    constexpr std::uint32_t singleChangedY     = 3U;
    constexpr std::uint64_t changedBlockPixels = 4U;
    constexpr std::uint64_t noDiffPixels       = 0U;
    constexpr std::uint64_t totalPixels = static_cast<std::uint64_t>( imageWidth ) *
                                          static_cast<std::uint64_t>( imageHeight );
    constexpr std::uint64_t changedBlockMatches = totalPixels - changedBlockPixels;
    constexpr double        fullMatchRatio      = 1.0;
    constexpr std::uint8_t  baseRed             = 40U;
    constexpr std::uint8_t  baseGreen           = 80U;
    constexpr std::uint8_t  baseBlue            = 120U;
    constexpr std::uint8_t  baseAlpha           = 160U;
    constexpr std::uint8_t  changedRed          = 190U;
    constexpr std::uint8_t  changedGreen        = 30U;
    constexpr std::uint8_t  changedBlue         = 210U;
    constexpr std::uint8_t  changedAlpha        = 250U;
    constexpr std::uint8_t  toleranceDelta      = 3U;
    constexpr std::uint8_t  diffRed             = 0XFFU;
    constexpr std::uint8_t  diffGreen           = 0U;
    constexpr std::uint8_t  diffBlue            = 0U;
    constexpr std::uint8_t  diffAlpha           = 0XFFU;
    constexpr std::size_t   redOffset           = 0U;
    constexpr std::size_t   greenOffset         = 1U;
    constexpr std::size_t   blueOffset          = 2U;
    constexpr std::size_t   alphaOffset         = 3U;
    constexpr auto          invalidArgument     = grab::ErrorCode::InvalidArgument;

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
               ( static_cast<std::size_t>( x ) * static_cast<std::size_t>( rgbaBytes ) );
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
        const auto offset                       = pixel_offset( image, x, y );
        image.pixels.at( offset + redOffset )   = byte_from( red );
        image.pixels.at( offset + greenOffset ) = byte_from( green );
        image.pixels.at( offset + blueOffset )  = byte_from( blue );
        image.pixels.at( offset + alphaOffset ) = byte_from( alpha );
    }

    [[nodiscard]]
    grab::Image
    make_rgba_image( std::uint32_t width,
                     std::uint32_t height )
    {
        const auto  stride = width * rgbaBytes;
        grab::Image image{
            .width  = width,
            .height = height,
            .stride = stride,
            .format = grab::PixelFormat::Rgba,
            .pixels = std::vector<std::byte>( static_cast<std::size_t>( stride ) *
                                              static_cast<std::size_t>( height ) ),
        };

        for( std::uint32_t y = 0U; y < height; ++y )
        {
            for( std::uint32_t x = 0U; x < width; ++x )
            {
                set_pixel( image, x, y, baseRed, baseGreen, baseBlue, baseAlpha );
            }
        }

        return image;
    }

    [[nodiscard]]
    double
    ratio( std::uint64_t matching_pixels ) noexcept
    {
        return static_cast<double>( matching_pixels ) /
               static_cast<double>( totalPixels );
    }

    void
    change_block( grab::Image& image )
    {
        for( std::uint32_t y = changedBlockY; y < changedBlockEndY; ++y )
        {
            for( std::uint32_t x = changedBlockX; x < changedBlockEndX; ++x )
            {
                set_pixel( image,
                           x,
                           y,
                           changedRed,
                           changedGreen,
                           changedBlue,
                           changedAlpha );
            }
        }
    }

    void
    add_tolerance_delta_to_all_pixels( grab::Image& image )
    {
        for( auto& pixel_byte : image.pixels )
        {
            const auto value =
                static_cast<std::uint8_t>( byte_value( pixel_byte ) + toleranceDelta );
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
    const auto first  = make_rgba_image( imageWidth, imageHeight );
    const auto second = make_rgba_image( imageWidth, imageHeight );

    const auto result = grab::image::compare( first, second );

    ASSERT_TRUE( result.has_value() );
    EXPECT_DOUBLE_EQ( result->match_ratio, fullMatchRatio );
    EXPECT_EQ( result->diff_pixels, noDiffPixels );
    EXPECT_FALSE( result->bounding_box.has_value() );
}

TEST( Compare,
      ChangedRegionIsCounted )
{
    const auto first  = make_rgba_image( imageWidth, imageHeight );
    auto       second = make_rgba_image( imageWidth, imageHeight );
    change_block( second );

    const auto result = grab::image::compare( first, second );

    ASSERT_TRUE( result.has_value() );
    EXPECT_DOUBLE_EQ( result->match_ratio, ratio( changedBlockMatches ) );
    EXPECT_EQ( result->diff_pixels, changedBlockPixels );
    ASSERT_TRUE( result->bounding_box.has_value() );
    expect_rect_eq( *result->bounding_box,
                    grab::image::Rect{
                        .x      = static_cast<std::int32_t>( changedBlockX ),
                        .y      = static_cast<std::int32_t>( changedBlockY ),
                        .width  = changedBlockWidth,
                        .height = changedBlockHeight,
                    } );
}

TEST( Compare,
      ToleranceAbsorbsSmallDeltas )
{
    const auto first  = make_rgba_image( imageWidth, imageHeight );
    auto       second = make_rgba_image( imageWidth, imageHeight );
    add_tolerance_delta_to_all_pixels( second );

    const auto tolerant = grab::image::compare(
        first,
        second,
        grab::image::CompareOptions{ .per_channel_tolerance = toleranceDelta }
    );
    ASSERT_TRUE( tolerant.has_value() );
    EXPECT_EQ( tolerant->diff_pixels, noDiffPixels );
    EXPECT_FALSE( tolerant->bounding_box.has_value() );

    const auto exact = grab::image::compare( first, second );
    ASSERT_TRUE( exact.has_value() );
    EXPECT_EQ( exact->diff_pixels, totalPixels );
    ASSERT_TRUE( exact->bounding_box.has_value() );
    expect_rect_eq( *exact->bounding_box,
                    grab::image::Rect{
                        .x      = originX,
                        .y      = originY,
                        .width  = imageWidth,
                        .height = imageHeight,
                    } );
}

TEST( Compare,
      MismatchedDimsRejected )
{
    const auto first  = make_rgba_image( imageWidth, imageHeight );
    const auto second = make_rgba_image( mismatchedWidth, imageHeight );

    const auto result = grab::image::compare( first, second );

    ASSERT_FALSE( result.has_value() );
    EXPECT_EQ( result.error().code, invalidArgument );
}

TEST( Diff,
      DiffImageHighlightsChanges )
{
    const auto first  = make_rgba_image( imageWidth, imageHeight );
    auto       second = make_rgba_image( imageWidth, imageHeight );
    set_pixel( second,
               singleChangedX,
               singleChangedY,
               changedRed,
               changedGreen,
               changedBlue,
               changedAlpha );

    const auto result = grab::image::diff_image( first, second );

    ASSERT_TRUE( result.has_value() );
    EXPECT_EQ( result->width, imageWidth );
    EXPECT_EQ( result->height, imageHeight );
    EXPECT_EQ( result->stride, rgbaStride );
    EXPECT_EQ( result->format, grab::PixelFormat::Rgba );

    const auto offset = pixel_offset( *result, singleChangedX, singleChangedY );
    EXPECT_EQ( result->pixels.at( offset + redOffset ), byte_from( diffRed ) );
    EXPECT_EQ( result->pixels.at( offset + greenOffset ), byte_from( diffGreen ) );
    EXPECT_EQ( result->pixels.at( offset + blueOffset ), byte_from( diffBlue ) );
    EXPECT_EQ( result->pixels.at( offset + alphaOffset ), byte_from( diffAlpha ) );
}
