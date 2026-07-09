#include "codec/png.hpp"
#include "grab/image.hpp"
#include "grab/result.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace
{

    constexpr std::uint32_t    kWidth                 = 3U;
    constexpr std::uint32_t    kHeight                = 2U;
    constexpr std::uint32_t    kRgbaBytes             = 4U;
    constexpr std::uint32_t    kRgbaStride            = kWidth * kRgbaBytes;
    constexpr std::size_t      kMinimumValidCorpus    = 2U;
    constexpr std::size_t      kMinimumInvalidCorpus  = 2U;
    constexpr std::size_t      kByteModulus           = 256U;
    constexpr std::size_t      kChannelStep           = 29U;
    constexpr std::size_t      kRedSeed               = 17U;
    constexpr std::size_t      kGreenSeed             = 83U;
    constexpr std::size_t      kBlueSeed              = 149U;
    constexpr std::size_t      kAlphaSeed             = 211U;
    constexpr std::size_t      kGraySeed              = 47U;
    constexpr std::size_t      kTruncatedPngBytes     = 12U;
    constexpr std::size_t      kPngSignatureBytes     = 8U;
    constexpr std::size_t      kPngChunkLengthBytes   = 4U;
    constexpr std::size_t      kPngChunkTypeBytes     = 4U;
    constexpr std::size_t      kPngIhdrDataBytes      = 13U;
    constexpr std::size_t      kPngCrcLastBytePadding = 3U;
    constexpr std::size_t      kIhdrCrcLastByteOffset = kPngSignatureBytes +
                                                        kPngChunkLengthBytes +
                                                        kPngChunkTypeBytes +
                                                        kPngIhdrDataBytes +
                                                        kPngCrcLastBytePadding;
    constexpr unsigned char    kCrcFlipMask           = 0XFFU;
    constexpr auto             kProtocolError         = grab::ErrorCode::protocol_error;
    constexpr std::string_view kValidPrefix           = "valid_";
    constexpr std::string_view kInvalidPrefix         = "invalid_";
    constexpr std::string_view kFilterSub             = "valid_filter_sub.png";
    constexpr std::string_view kFilterUp              = "valid_filter_up.png";
    constexpr std::string_view kFilterAverage         = "valid_filter_average.png";
    constexpr std::string_view kFilterPaeth           = "valid_filter_paeth.png";
    constexpr std::string_view kPaletteTransparent    = "valid_palette_trns.png";

    [[nodiscard]]
    std::byte
    byte_from( std::size_t value ) noexcept
    {
        return static_cast<std::byte>( static_cast<unsigned char>( value %
                                                                   kByteModulus ) );
    }

    [[nodiscard]]
    std::byte
    channel_byte( std::size_t pixel_index,
                  std::size_t seed ) noexcept
    {
        return byte_from( seed + ( pixel_index * kChannelStep ) );
    }

    void
    append_rgb_pixel( std::vector<std::byte>& pixels,
                      std::size_t             pixel_index )
    {
        pixels.push_back( channel_byte( pixel_index, kRedSeed ) );
        pixels.push_back( channel_byte( pixel_index, kGreenSeed ) );
        pixels.push_back( channel_byte( pixel_index, kBlueSeed ) );
    }

    void
    append_bgr_pixel( std::vector<std::byte>& pixels,
                      std::size_t             pixel_index )
    {
        pixels.push_back( channel_byte( pixel_index, kBlueSeed ) );
        pixels.push_back( channel_byte( pixel_index, kGreenSeed ) );
        pixels.push_back( channel_byte( pixel_index, kRedSeed ) );
    }

    void
    append_rgba_pixel( std::vector<std::byte>& pixels,
                       std::size_t             pixel_index )
    {
        append_rgb_pixel( pixels, pixel_index );
        pixels.push_back( channel_byte( pixel_index, kAlphaSeed ) );
    }

    void
    append_bgra_pixel( std::vector<std::byte>& pixels,
                       std::size_t             pixel_index )
    {
        append_bgr_pixel( pixels, pixel_index );
        pixels.push_back( channel_byte( pixel_index, kAlphaSeed ) );
    }

    [[nodiscard]]
    std::size_t
    pixel_count() noexcept
    {
        return static_cast<std::size_t>( kWidth ) * static_cast<std::size_t>( kHeight );
    }

    [[nodiscard]]
    grab::Image
    make_pattern( grab::PixelFormat format )
    {
        std::vector<std::byte> pixels;
        pixels.reserve( pixel_count() *
                        static_cast<std::size_t>( grab::bytes_per_pixel( format ) ) );

        for( std::size_t pixel_index = 0U; pixel_index < pixel_count(); ++pixel_index )
        {
            switch( format )
            {
                case grab::PixelFormat::gray :
                    pixels.push_back( channel_byte( pixel_index, kGraySeed ) );
                    break;
                case grab::PixelFormat::rgb :
                case grab::PixelFormat::rgb24 :
                    append_rgb_pixel( pixels, pixel_index );
                    break;
                case grab::PixelFormat::bgr :
                    append_bgr_pixel( pixels, pixel_index );
                    break;
                case grab::PixelFormat::rgba :
                    append_rgba_pixel( pixels, pixel_index );
                    break;
                case grab::PixelFormat::bgra :
                case grab::PixelFormat::bgr0 :
                    append_bgra_pixel( pixels, pixel_index );
                    break;
            }
        }

        return grab::Image{
            .width  = kWidth,
            .height = kHeight,
            .stride = kWidth * grab::bytes_per_pixel( format ),
            .format = format,
            .pixels = std::move( pixels ),
        };
    }

    [[nodiscard]]
    grab::Image
    expected_png_image( grab::PixelFormat source_format )
    {
        if( source_format == grab::PixelFormat::bgr )
        {
            return make_pattern( grab::PixelFormat::rgb );
        }
        if( source_format == grab::PixelFormat::bgra )
        {
            return make_pattern( grab::PixelFormat::rgba );
        }
        return make_pattern( source_format );
    }

    void
    expect_image_eq( const grab::Image& actual,
                     const grab::Image& expected )
    {
        EXPECT_EQ( actual.width, expected.width );
        EXPECT_EQ( actual.height, expected.height );
        EXPECT_EQ( actual.stride, expected.stride );
        EXPECT_EQ( actual.format, expected.format );
        EXPECT_EQ( actual.pixels, expected.pixels );
    }

    [[nodiscard]]
    std::string_view
    format_name( grab::PixelFormat format )
    {
        switch( format )
        {
            case grab::PixelFormat::bgra :
                return "bgra";
            case grab::PixelFormat::rgba :
                return "rgba";
            case grab::PixelFormat::rgb :
                return "rgb";
            case grab::PixelFormat::bgr :
                return "bgr";
            case grab::PixelFormat::rgb24 :
                return "rgb24";
            case grab::PixelFormat::bgr0 :
                return "bgr0";
            case grab::PixelFormat::gray :
                return "gray";
        }
        return "unknown";
    }

    [[nodiscard]]
    std::vector<std::byte>
    read_file( const std::filesystem::path& path )
    {
        std::ifstream     stream{ path, std::ios::binary };
        std::vector<char> bytes{
            std::istreambuf_iterator<char>{ stream },
            std::istreambuf_iterator<char>{}
        };

        std::vector<std::byte> result;
        result.reserve( bytes.size() );
        std::ranges::transform(
            bytes,
            std::back_inserter( result ),
            []( char value )
            {
                return static_cast<std::byte>( static_cast<unsigned char>( value ) );
            }
        );
        return result;
    }

    [[nodiscard]]
    std::filesystem::path
    corpus_path( std::string_view filename )
    {
        return std::filesystem::path{ GRAB_CODEC_CORPUS_DIR } / std::string{ filename };
    }

    void
    expect_protocol_error( std::span<const std::byte> bytes )
    {
        const auto decoded = grab::codec::decode_png( bytes );
        ASSERT_FALSE( decoded.has_value() );
        EXPECT_EQ( decoded.error().code, kProtocolError );
    }

    void
    expect_plausible_image( const grab::Image& image )
    {
        ASSERT_FALSE( image.empty() );
        const auto min_stride = image.width * grab::bytes_per_pixel( image.format );
        EXPECT_GE( image.stride, min_stride );
        EXPECT_GE( image.pixels.size(),
                   static_cast<std::size_t>( image.stride ) *
                       static_cast<std::size_t>( image.height ) );
    }

    struct CorpusCounts
    {
            std::size_t valid   = 0U;
            std::size_t invalid = 0U;
    };

    void
    expect_valid_corpus_file( const std::filesystem::path& path,
                              CorpusCounts&                counts )
    {
        ++counts.valid;
        const auto decoded = grab::codec::decode_png( read_file( path ) );
        ASSERT_TRUE( decoded.has_value() ) << path << ": " << decoded.error().message;
        expect_plausible_image( *decoded );
    }

    void
    expect_invalid_corpus_file( const std::filesystem::path& path,
                                CorpusCounts&                counts )
    {
        ++counts.invalid;
        const auto decoded = grab::codec::decode_png( read_file( path ) );
        ASSERT_FALSE( decoded.has_value() ) << path;
        EXPECT_EQ( decoded.error().code, kProtocolError );
    }

    void
    check_corpus_entry( const std::filesystem::directory_entry& entry,
                        CorpusCounts&                           counts )
    {
        if( !entry.is_regular_file() )
        {
            return;
        }

        const auto filename = entry.path().filename().string();
        if( filename.starts_with( kValidPrefix ) )
        {
            expect_valid_corpus_file( entry.path(), counts );
            return;
        }

        if( filename.starts_with( kInvalidPrefix ) )
        {
            expect_invalid_corpus_file( entry.path(), counts );
        }
    }

}    // namespace

TEST( PngCodec,
      RoundTripsEachSupportedPixelFormat )
{
    constexpr std::array formats{
        grab::PixelFormat::gray,
        grab::PixelFormat::rgb,
        grab::PixelFormat::rgba,
        grab::PixelFormat::bgr,
        grab::PixelFormat::bgra,
    };

    for( const auto format : formats )
    {
        SCOPED_TRACE( format_name( format ) );
        const auto image   = make_pattern( format );
        const auto encoded = grab::codec::encode_png( image );
        ASSERT_TRUE( encoded.has_value() ) << encoded.error().message;

        const auto decoded = grab::codec::decode_png( *encoded );
        ASSERT_TRUE( decoded.has_value() ) << decoded.error().message;
        expect_image_eq( *decoded, expected_png_image( format ) );
    }
}

TEST( PngCodec,
      RejectsGarbageAndTruncatedStreams )
{
    const std::vector<std::byte> garbage{
        byte_from( kRedSeed ),
        byte_from( kGreenSeed ),
        byte_from( kBlueSeed ),
    };
    expect_protocol_error( garbage );

    const auto image   = make_pattern( grab::PixelFormat::rgb );
    const auto encoded = grab::codec::encode_png( image );
    ASSERT_TRUE( encoded.has_value() );

    auto truncated = *encoded;
    truncated.resize( kTruncatedPngBytes );
    expect_protocol_error( truncated );
}

TEST( PngCodec,
      RejectsBadChunkCrc )
{
    const auto image   = make_pattern( grab::PixelFormat::rgba );
    const auto encoded = grab::codec::encode_png( image );
    ASSERT_TRUE( encoded.has_value() );

    auto corrupted = *encoded;
    ASSERT_GT( corrupted.size(), kIhdrCrcLastByteOffset );
    corrupted.at( kIhdrCrcLastByteOffset ) =
        static_cast<std::byte>( static_cast<unsigned char>(
            std::to_integer<unsigned char>( corrupted.at( kIhdrCrcLastByteOffset ) ) ^
            kCrcFlipMask
        ) );

    expect_protocol_error( corrupted );
}

TEST( PngCodec,
      DecodesAllStandardFiltersFromCorpus )
{
    const std::array filter_files{
        kFilterSub,
        kFilterUp,
        kFilterAverage,
        kFilterPaeth,
    };
    const auto expected = make_pattern( grab::PixelFormat::rgb );

    for( const auto filename : filter_files )
    {
        SCOPED_TRACE( std::string{ filename } );
        const auto decoded =
            grab::codec::decode_png( read_file( corpus_path( filename ) ) );
        ASSERT_TRUE( decoded.has_value() ) << decoded.error().message;
        expect_image_eq( *decoded, expected );
    }
}

TEST( PngCodec,
      DecodesPaletteTransparencyFromCorpus )
{
    const auto decoded =
        grab::codec::decode_png( read_file( corpus_path( kPaletteTransparent ) ) );
    ASSERT_TRUE( decoded.has_value() ) << decoded.error().message;
    EXPECT_EQ( decoded->width, kWidth );
    EXPECT_EQ( decoded->height, kHeight );
    EXPECT_EQ( decoded->stride, kRgbaStride );
    EXPECT_EQ( decoded->format, grab::PixelFormat::rgba );
    EXPECT_EQ( decoded->pixels.size(),
               static_cast<std::size_t>( kRgbaStride ) *
                   static_cast<std::size_t>( kHeight ) );
}

TEST( PngCorpus,
      ValidFilesDecodeAndAdversarialFilesRejectCleanly )
{
    const std::filesystem::path corpus_dir{ GRAB_CODEC_CORPUS_DIR };
    ASSERT_TRUE( std::filesystem::is_directory( corpus_dir ) );

    CorpusCounts counts;
    for( const auto& entry : std::filesystem::directory_iterator{ corpus_dir } )
    {
        check_corpus_entry( entry, counts );
    }

    EXPECT_GE( counts.valid, kMinimumValidCorpus );
    EXPECT_GE( counts.invalid, kMinimumInvalidCorpus );
}
