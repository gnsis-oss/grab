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

    constexpr std::uint32_t    width                 = 3U;
    constexpr std::uint32_t    height                = 2U;
    constexpr std::uint32_t    rgbaBytes             = 4U;
    constexpr std::uint32_t    rgbaStride            = width * rgbaBytes;
    constexpr std::size_t      minimumValidCorpus    = 2U;
    constexpr std::size_t      minimumInvalidCorpus  = 2U;
    constexpr std::size_t      byteModulus           = 256U;
    constexpr std::size_t      channelStep           = 29U;
    constexpr std::size_t      redSeed               = 17U;
    constexpr std::size_t      greenSeed             = 83U;
    constexpr std::size_t      blueSeed              = 149U;
    constexpr std::size_t      alphaSeed             = 211U;
    constexpr std::size_t      graySeed              = 47U;
    constexpr std::size_t      truncatedPngBytes     = 12U;
    constexpr std::size_t      pngSignatureBytes     = 8U;
    constexpr std::size_t      pngChunkLengthBytes   = 4U;
    constexpr std::size_t      pngChunkTypeBytes     = 4U;
    constexpr std::size_t      pngIhdrDataBytes      = 13U;
    constexpr std::size_t      pngCrcLastBytePadding = 3U;
    constexpr std::size_t      ihdrCrcLastByteOffset = pngSignatureBytes +
                                                       pngChunkLengthBytes +
                                                       pngChunkTypeBytes +
                                                       pngIhdrDataBytes +
                                                       pngCrcLastBytePadding;
    constexpr unsigned char    crcFlipMask           = 0XFFU;
    constexpr auto             protocolError         = grab::ErrorCode::ProtocolError;
    constexpr std::string_view validPrefix           = "valid_";
    constexpr std::string_view invalidPrefix         = "invalid_";
    constexpr std::string_view filterSub             = "valid_filter_sub.png";
    constexpr std::string_view filterUp              = "valid_filter_up.png";
    constexpr std::string_view filterAverage         = "valid_filter_average.png";
    constexpr std::string_view filterPaeth           = "valid_filter_paeth.png";
    constexpr std::string_view paletteTransparent    = "valid_palette_trns.png";

    [[nodiscard]]
    std::byte
    byte_from( std::size_t value ) noexcept
    {
        return static_cast<std::byte>( static_cast<unsigned char>( value %
                                                                   byteModulus ) );
    }

    [[nodiscard]]
    std::byte
    channel_byte( std::size_t pixel_index,
                  std::size_t seed ) noexcept
    {
        return byte_from( seed + ( pixel_index * channelStep ) );
    }

    void
    append_rgb_pixel( std::vector<std::byte>& pixels,
                      std::size_t             pixel_index )
    {
        pixels.push_back( channel_byte( pixel_index, redSeed ) );
        pixels.push_back( channel_byte( pixel_index, greenSeed ) );
        pixels.push_back( channel_byte( pixel_index, blueSeed ) );
    }

    void
    append_bgr_pixel( std::vector<std::byte>& pixels,
                      std::size_t             pixel_index )
    {
        pixels.push_back( channel_byte( pixel_index, blueSeed ) );
        pixels.push_back( channel_byte( pixel_index, greenSeed ) );
        pixels.push_back( channel_byte( pixel_index, redSeed ) );
    }

    void
    append_rgba_pixel( std::vector<std::byte>& pixels,
                       std::size_t             pixel_index )
    {
        append_rgb_pixel( pixels, pixel_index );
        pixels.push_back( channel_byte( pixel_index, alphaSeed ) );
    }

    void
    append_bgra_pixel( std::vector<std::byte>& pixels,
                       std::size_t             pixel_index )
    {
        append_bgr_pixel( pixels, pixel_index );
        pixels.push_back( channel_byte( pixel_index, alphaSeed ) );
    }

    [[nodiscard]]
    std::size_t
    pixel_count() noexcept
    {
        return static_cast<std::size_t>( width ) * static_cast<std::size_t>( height );
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
                case grab::PixelFormat::Gray :
                    pixels.push_back( channel_byte( pixel_index, graySeed ) );
                    break;
                case grab::PixelFormat::Rgb :
                    append_rgb_pixel( pixels, pixel_index );
                    break;
                case grab::PixelFormat::Bgr :
                    append_bgr_pixel( pixels, pixel_index );
                    break;
                case grab::PixelFormat::Rgba :
                    append_rgba_pixel( pixels, pixel_index );
                    break;
                case grab::PixelFormat::Bgra :
                    append_bgra_pixel( pixels, pixel_index );
                    break;
            }
        }

        return grab::Image{
            .width  = width,
            .height = height,
            .stride = width * grab::bytes_per_pixel( format ),
            .format = format,
            .pixels = std::move( pixels ),
        };
    }

    [[nodiscard]]
    grab::Image
    expected_png_image( grab::PixelFormat source_format )
    {
        if( source_format == grab::PixelFormat::Bgr )
        {
            return make_pattern( grab::PixelFormat::Rgb );
        }
        if( source_format == grab::PixelFormat::Bgra )
        {
            return make_pattern( grab::PixelFormat::Rgba );
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
            case grab::PixelFormat::Bgra :
                return "bgra";
            case grab::PixelFormat::Rgba :
                return "rgba";
            case grab::PixelFormat::Rgb :
                return "rgb";
            case grab::PixelFormat::Bgr :
                return "bgr";
            case grab::PixelFormat::Gray :
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
        EXPECT_EQ( decoded.error().code, protocolError );
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
        EXPECT_EQ( decoded.error().code, protocolError );
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
        if( filename.starts_with( validPrefix ) )
        {
            expect_valid_corpus_file( entry.path(), counts );
            return;
        }

        if( filename.starts_with( invalidPrefix ) )
        {
            expect_invalid_corpus_file( entry.path(), counts );
        }
    }

}    // namespace

TEST( PngCodec,
      RoundTripsEachSupportedPixelFormat )
{
    constexpr std::array formats{
        grab::PixelFormat::Gray,
        grab::PixelFormat::Rgb,
        grab::PixelFormat::Rgba,
        grab::PixelFormat::Bgr,
        grab::PixelFormat::Bgra,
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
        byte_from( redSeed ),
        byte_from( greenSeed ),
        byte_from( blueSeed ),
    };
    expect_protocol_error( garbage );

    const auto image   = make_pattern( grab::PixelFormat::Rgb );
    const auto encoded = grab::codec::encode_png( image );
    ASSERT_TRUE( encoded.has_value() );

    auto truncated = *encoded;
    truncated.resize( truncatedPngBytes );
    expect_protocol_error( truncated );
}

TEST( PngCodec,
      RejectsBadChunkCrc )
{
    const auto image   = make_pattern( grab::PixelFormat::Rgba );
    const auto encoded = grab::codec::encode_png( image );
    ASSERT_TRUE( encoded.has_value() );

    auto corrupted = *encoded;
    ASSERT_GT( corrupted.size(), ihdrCrcLastByteOffset );
    corrupted.at( ihdrCrcLastByteOffset ) =
        static_cast<std::byte>( static_cast<unsigned char>(
            std::to_integer<unsigned char>( corrupted.at( ihdrCrcLastByteOffset ) ) ^
            crcFlipMask
        ) );

    expect_protocol_error( corrupted );
}

TEST( PngCodec,
      DecodesAllStandardFiltersFromCorpus )
{
    const std::array filter_files{
        filterSub,
        filterUp,
        filterAverage,
        filterPaeth,
    };
    const auto expected = make_pattern( grab::PixelFormat::Rgb );

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
        grab::codec::decode_png( read_file( corpus_path( paletteTransparent ) ) );
    ASSERT_TRUE( decoded.has_value() ) << decoded.error().message;
    EXPECT_EQ( decoded->width, width );
    EXPECT_EQ( decoded->height, height );
    EXPECT_EQ( decoded->stride, rgbaStride );
    EXPECT_EQ( decoded->format, grab::PixelFormat::Rgba );
    EXPECT_EQ( decoded->pixels.size(),
               static_cast<std::size_t>( rgbaStride ) *
                   static_cast<std::size_t>( height ) );
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

    EXPECT_GE( counts.valid, minimumValidCorpus );
    EXPECT_GE( counts.invalid, minimumInvalidCorpus );
}
