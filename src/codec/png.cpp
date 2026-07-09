#include "codec/png.hpp"
#include "grab/image.hpp"
#include "grab/result.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <zconf.h>
#include <zlib.h>

namespace grab::codec
{
    namespace
    {

        using Byte                                        = std::uint8_t;
        using ByteVector                                  = std::vector<Byte>;
        using ChunkType                                   = std::array<Byte, 4U>;

        constexpr Byte                 kPngSignatureByte0 = 0X89U;
        constexpr Byte                 kPngSignatureByte1 = 0X50U;
        constexpr Byte                 kPngSignatureByte2 = 0X4EU;
        constexpr Byte                 kPngSignatureByte3 = 0X47U;
        constexpr Byte                 kPngSignatureByte4 = 0X0DU;
        constexpr Byte                 kPngSignatureByte5 = 0X0AU;
        constexpr Byte                 kPngSignatureByte6 = 0X1AU;
        constexpr Byte                 kPngSignatureByte7 = 0X0AU;
        constexpr std::array<Byte, 8U> kPngSignature      = {
            kPngSignatureByte0,
            kPngSignatureByte1,
            kPngSignatureByte2,
            kPngSignatureByte3,
            kPngSignatureByte4,
            kPngSignatureByte5,
            kPngSignatureByte6,
            kPngSignatureByte7,
        };
        constexpr ChunkType   kChunkIhdr           = { 'I', 'H', 'D', 'R' };
        constexpr ChunkType   kChunkPlte           = { 'P', 'L', 'T', 'E' };
        constexpr ChunkType   kChunkIdat           = { 'I', 'D', 'A', 'T' };
        constexpr ChunkType   kChunkIend           = { 'I', 'E', 'N', 'D' };
        constexpr ChunkType   kChunkTrns           = { 't', 'R', 'N', 'S' };
        constexpr std::size_t kPngUint32Bytes      = 4U;
        constexpr std::size_t kPngChunkLengthBytes = 4U;
        constexpr std::size_t kPngChunkTypeBytes   = 4U;
        constexpr std::size_t kPngChunkCrcBytes    = 4U;
        constexpr std::size_t kPngChunkOverheadBytes =
            kPngChunkLengthBytes + kPngChunkTypeBytes + kPngChunkCrcBytes;
        constexpr std::size_t      kPngIhdrBytes          = 13U;
        constexpr std::size_t      kIhdrWidthOffset       = 0U;
        constexpr std::size_t      kIhdrHeightOffset      = 4U;
        constexpr std::size_t      kIhdrBitDepthOffset    = 8U;
        constexpr std::size_t      kIhdrColorTypeOffset   = 9U;
        constexpr std::size_t      kIhdrCompressionOffset = 10U;
        constexpr std::size_t      kIhdrFilterOffset      = 11U;
        constexpr std::size_t      kIhdrInterlaceOffset   = 12U;
        constexpr Byte             kPngBitDepth8          = 8U;
        constexpr Byte             kPngMethodNone         = 0U;
        constexpr Byte             kPngColorGray          = 0U;
        constexpr Byte             kPngColorRgb           = 2U;
        constexpr Byte             kPngColorPalette       = 3U;
        constexpr Byte             kPngColorGrayAlpha     = 4U;
        constexpr Byte             kPngColorRgba          = 6U;
        constexpr std::size_t      kGrayBytes             = 1U;
        constexpr std::size_t      kRgbBytes              = 3U;
        constexpr std::size_t      kRgbaBytes             = 4U;
        constexpr std::size_t      kPaletteEntryBytes     = 3U;
        constexpr std::size_t      kMaxPaletteEntries     = 256U;
        constexpr Byte             kOpaqueAlpha           = 0XFFU;
        constexpr unsigned int     kByteModulo            = 256U;
        constexpr std::uint32_t    kByteMask              = 0XFFU;
        constexpr std::uint32_t    kBigEndianShift24      = 24U;
        constexpr std::uint32_t    kBigEndianShift16      = 16U;
        constexpr std::uint32_t    kBigEndianShift8       = 8U;
        constexpr std::size_t      kKilobyte              = 1'024U;
        constexpr std::size_t      kMaxChunkBytes         = 64U * kKilobyte * kKilobyte;
        constexpr std::size_t      kMaxCompressedBytes    = 64U * kKilobyte * kKilobyte;
        constexpr std::size_t      kMaxDecodedBytes       = 256U * kKilobyte * kKilobyte;
        constexpr uLong            kInitialCrc            = 0UL;
        constexpr Byte             kAsciiUpperA           = 0X41U;
        constexpr Byte             kAsciiUpperZ           = 0X5AU;
        constexpr Byte             kAsciiLowerA           = 0X61U;
        constexpr Byte             kAsciiLowerZ           = 0X7AU;
        constexpr std::string_view kMalformedPngPrefix    = "malformed PNG: ";
        constexpr std::string_view kInvalidImagePrefix    = "invalid image: ";
        constexpr std::string_view kCodecFailurePrefix    = "PNG codec failure: ";

        enum class PngColorType : Byte
        {
            grayscale       = kPngColorGray,
            truecolor       = kPngColorRgb,
            palette         = kPngColorPalette,
            grayscale_alpha = kPngColorGrayAlpha,
            truecolor_alpha = kPngColorRgba,
        };

        enum class FilterType : Byte
        {
            none    = 0U,
            sub     = 1U,
            up      = 2U,
            average = 3U,
            paeth   = 4U,
        };

        struct Ihdr
        {
                std::uint32_t width       = 0U;
                std::uint32_t height      = 0U;
                Byte          bit_depth   = 0U;
                PngColorType  color_type  = PngColorType::truecolor_alpha;
                Byte          compression = 0U;
                Byte          filter      = 0U;
                Byte          interlace   = 0U;
        };

        struct ParsedPng
        {
                Ihdr       ihdr;
                ByteVector idat;
                ByteVector palette;
                ByteVector transparency;
        };

        struct ParseState
        {
                ParsedPng png;
                bool      seen_ihdr = false;
                bool      seen_idat = false;
        };

        struct EncodeLayout
        {
                std::size_t  source_row_bytes   = 0U;
                std::size_t  png_row_bytes      = 0U;
                std::size_t  filtered_bytes     = 0U;
                std::size_t  source_pixel_bytes = 0U;
                PngColorType color_type         = PngColorType::truecolor_alpha;
        };

        struct DecodeLayout
        {
                std::size_t       source_row_bytes   = 0U;
                std::size_t       filtered_row_bytes = 0U;
                std::size_t       filtered_bytes     = 0U;
                std::size_t       filter_pixel_bytes = 0U;
                std::size_t       output_stride      = 0U;
                std::size_t       output_bytes       = 0U;
                grab::PixelFormat output_format      = grab::PixelFormat::rgba;
        };

        [[nodiscard]]
        std::unexpected<grab::Error>
        png_protocol_error( std::string message )
        {
            return grab::fail( grab::ErrorCode::protocol_error,
                               std::string{ kMalformedPngPrefix } +
                                   std::move( message ) );
        }

        [[nodiscard]]
        std::unexpected<grab::Error>
        invalid_image_error( std::string message )
        {
            return grab::fail( grab::ErrorCode::invalid_argument,
                               std::string{ kInvalidImagePrefix } +
                                   std::move( message ) );
        }

        [[nodiscard]]
        std::unexpected<grab::Error>
        codec_failure( std::string message )
        {
            return grab::fail( grab::ErrorCode::internal_fault,
                               std::string{ kCodecFailurePrefix } +
                                   std::move( message ) );
        }

        [[nodiscard]]
        Byte
        to_u8( std::byte value ) noexcept
        {
            return std::to_integer<Byte>( value );
        }

        [[nodiscard]]
        std::byte
        to_std_byte( Byte value ) noexcept
        {
            return static_cast<std::byte>( value );
        }

        [[nodiscard]]
        std::vector<std::byte>
        bytes_from_u8( const ByteVector& bytes )
        {
            std::vector<std::byte> result;
            result.reserve( bytes.size() );
            std::ranges::transform( bytes, std::back_inserter( result ), to_std_byte );
            return result;
        }

        [[nodiscard]]
        ByteVector
        bytes_to_u8( std::span<const std::byte> bytes )
        {
            ByteVector result;
            result.reserve( bytes.size() );
            std::ranges::transform( bytes, std::back_inserter( result ), to_u8 );
            return result;
        }

        [[nodiscard]]
        bool
        checked_mul( std::size_t  left,
                     std::size_t  right,
                     std::size_t& result ) noexcept
        {
            constexpr auto kMaxSize = std::numeric_limits<std::size_t>::max();
            if( right != 0U && left > kMaxSize / right )
            {
                return false;
            }

            result = left * right;
            return true;
        }

        [[nodiscard]]
        bool
        checked_add( std::size_t  left,
                     std::size_t  right,
                     std::size_t& result ) noexcept
        {
            constexpr auto kMaxSize = std::numeric_limits<std::size_t>::max();
            if( left > kMaxSize - right )
            {
                return false;
            }

            result = left + right;
            return true;
        }

        [[nodiscard]]
        bool
        has_bytes( std::span<const Byte> bytes,
                   std::size_t           offset,
                   std::size_t           count ) noexcept
        {
            return offset <= bytes.size() && bytes.size() - offset >= count;
        }

        [[nodiscard]]
        std::optional<std::uint32_t>
        read_be32( std::span<const Byte> bytes,
                   std::size_t           offset )
        {
            if( !has_bytes( bytes, offset, kPngUint32Bytes ) )
            {
                return std::nullopt;
            }

            std::uint32_t value = 0U;
            for( const auto byte : bytes.subspan( offset, kPngUint32Bytes ) )
            {
                value =
                    ( value << kBigEndianShift8 ) | static_cast<std::uint32_t>( byte );
            }
            return value;
        }

        [[nodiscard]]
        std::optional<Byte>
        read_byte( std::span<const Byte> bytes,
                   std::size_t           offset )
        {
            if( !has_bytes( bytes, offset, 1U ) )
            {
                return std::nullopt;
            }

            return bytes.subspan( offset, 1U ).front();
        }

        void
        append_be32( ByteVector&   output,
                     std::uint32_t value )
        {
            output.push_back( static_cast<Byte>( ( value >> kBigEndianShift24 ) &
                                                 kByteMask ) );
            output.push_back( static_cast<Byte>( ( value >> kBigEndianShift16 ) &
                                                 kByteMask ) );
            output.push_back( static_cast<Byte>( ( value >> kBigEndianShift8 ) &
                                                 kByteMask ) );
            output.push_back( static_cast<Byte>( value & kByteMask ) );
        }

        [[nodiscard]]
        std::optional<PngColorType>
        parse_color_type( Byte value ) noexcept
        {
            switch( value )
            {
                case kPngColorGray :
                    return PngColorType::grayscale;
                case kPngColorRgb :
                    return PngColorType::truecolor;
                case kPngColorPalette :
                    return PngColorType::palette;
                case kPngColorGrayAlpha :
                    return PngColorType::grayscale_alpha;
                case kPngColorRgba :
                    return PngColorType::truecolor_alpha;
                default :
                    return std::nullopt;
            }
        }

        [[nodiscard]]
        std::optional<FilterType>
        parse_filter_type( Byte value ) noexcept
        {
            switch( value )
            {
                case static_cast<Byte>( FilterType::none ) :
                    return FilterType::none;
                case static_cast<Byte>( FilterType::sub ) :
                    return FilterType::sub;
                case static_cast<Byte>( FilterType::up ) :
                    return FilterType::up;
                case static_cast<Byte>( FilterType::average ) :
                    return FilterType::average;
                case static_cast<Byte>( FilterType::paeth ) :
                    return FilterType::paeth;
                default :
                    return std::nullopt;
            }
        }

        [[nodiscard]]
        bool
        is_ascii_letter( Byte value ) noexcept
        {
            return ( value >= kAsciiUpperA && value <= kAsciiUpperZ ) ||
                   ( value >= kAsciiLowerA && value <= kAsciiLowerZ );
        }

        [[nodiscard]]
        bool
        is_chunk_type_valid( const ChunkType& type ) noexcept
        {
            return std::ranges::all_of( type, is_ascii_letter );
        }

        [[nodiscard]]
        bool
        is_critical_chunk( const ChunkType& type ) noexcept
        {
            return type.front() >= kAsciiUpperA && type.front() <= kAsciiUpperZ;
        }

        [[nodiscard]]
        std::optional<ChunkType>
        read_chunk_type( std::span<const Byte> bytes,
                         std::size_t           offset )
        {
            if( !has_bytes( bytes, offset, kPngChunkTypeBytes ) )
            {
                return std::nullopt;
            }

            ChunkType type{};
            std::ranges::copy( bytes.subspan( offset, kPngChunkTypeBytes ),
                               type.begin() );
            return type;
        }

        [[nodiscard]]
        std::uint32_t
        chunk_crc( const ChunkType&      type,
                   std::span<const Byte> data )
        {
            auto crc = crc32( kInitialCrc,
                              type.data(),
                              static_cast<uInt>( kPngChunkTypeBytes ) );
            if( !data.empty() )
            {
                crc = crc32( crc, data.data(), static_cast<uInt>( data.size() ) );
            }
            return static_cast<std::uint32_t>( crc );
        }

        [[nodiscard]]
        grab::Result<void>
        append_chunk( ByteVector&           output,
                      const ChunkType&      type,
                      std::span<const Byte> data )
        {
            if( data.size() >
                static_cast<std::size_t>( std::numeric_limits<std::uint32_t>::max() ) ||
                data.size() > kMaxChunkBytes )
            {
                return codec_failure( "chunk is too large" );
            }

            append_be32( output, static_cast<std::uint32_t>( data.size() ) );
            output.insert( output.end(), type.begin(), type.end() );
            output.insert( output.end(), data.begin(), data.end() );
            append_be32( output, chunk_crc( type, data ) );
            return {};
        }

        [[nodiscard]]
        grab::Result<EncodeLayout>
        validate_encode_image( const grab::Image& image )
        {
            if( image.empty() )
            {
                return invalid_image_error( "width and height must be non-zero" );
            }

            const auto source_pixel_bytes =
                static_cast<std::size_t>( grab::bytes_per_pixel( image.format ) );
            const auto  width            = static_cast<std::size_t>( image.width );
            const auto  height           = static_cast<std::size_t>( image.height );
            std::size_t source_row_bytes = 0U;
            if( !checked_mul( width, source_pixel_bytes, source_row_bytes ) )
            {
                return invalid_image_error( "source row size overflows" );
            }

            if( static_cast<std::size_t>( image.stride ) < source_row_bytes )
            {
                return invalid_image_error( "stride is smaller than the pixel row" );
            }

            std::size_t required_pixels = 0U;
            if( !checked_mul( height,
                              static_cast<std::size_t>( image.stride ),
                              required_pixels ) )
            {
                return invalid_image_error( "pixel buffer size overflows" );
            }

            if( image.pixels.size() < required_pixels )
            {
                return invalid_image_error(
                    "pixel buffer is shorter than height*stride"
                );
            }

            EncodeLayout layout{
                .source_row_bytes   = source_row_bytes,
                .png_row_bytes      = source_row_bytes,
                .filtered_bytes     = 0U,
                .source_pixel_bytes = source_pixel_bytes,
                .color_type         = PngColorType::truecolor_alpha,
            };
            switch( image.format )
            {
                case grab::PixelFormat::gray :
                    layout.color_type    = PngColorType::grayscale;
                    layout.png_row_bytes = width * kGrayBytes;
                    break;
                case grab::PixelFormat::rgb :
                case grab::PixelFormat::bgr :
                case grab::PixelFormat::rgb24 :
                    layout.color_type    = PngColorType::truecolor;
                    layout.png_row_bytes = width * kRgbBytes;
                    break;
                case grab::PixelFormat::rgba :
                case grab::PixelFormat::bgra :
                case grab::PixelFormat::bgr0 :
                    layout.color_type    = PngColorType::truecolor_alpha;
                    layout.png_row_bytes = width * kRgbaBytes;
                    break;
            }

            std::size_t filtered_row_bytes = 0U;
            if( !checked_add( layout.png_row_bytes, 1U, filtered_row_bytes ) ||
                !checked_mul( filtered_row_bytes, height, layout.filtered_bytes ) )
            {
                return invalid_image_error( "filtered scanline size overflows" );
            }

            if( layout.filtered_bytes > kMaxDecodedBytes )
            {
                return invalid_image_error( "image is too large to encode" );
            }

            return layout;
        }

        [[nodiscard]]
        Byte
        source_byte_at( std::span<const std::byte> row,
                        std::size_t                offset )
        {
            return to_u8( row.subspan( offset, 1U ).front() );
        }

        void
        append_direct_row( ByteVector&                scanlines,
                           std::span<const std::byte> row,
                           std::size_t                row_bytes )
        {
            for( const auto value : row.subspan( 0U, row_bytes ) )
            {
                scanlines.push_back( to_u8( value ) );
            }
        }

        void
        append_bgr_row( ByteVector&                scanlines,
                        std::span<const std::byte> row,
                        const EncodeLayout&        layout )
        {
            for( std::size_t offset  = 0U; offset < layout.source_row_bytes;
                 offset             += layout.source_pixel_bytes )
            {
                scanlines.push_back( source_byte_at( row, offset + 2U ) );
                scanlines.push_back( source_byte_at( row, offset + 1U ) );
                scanlines.push_back( source_byte_at( row, offset ) );
            }
        }

        void
        append_bgra_row( ByteVector&                scanlines,
                         std::span<const std::byte> row,
                         const EncodeLayout&        layout )
        {
            for( std::size_t offset  = 0U; offset < layout.source_row_bytes;
                 offset             += layout.source_pixel_bytes )
            {
                scanlines.push_back( source_byte_at( row, offset + 2U ) );
                scanlines.push_back( source_byte_at( row, offset + 1U ) );
                scanlines.push_back( source_byte_at( row, offset ) );
                scanlines.push_back( source_byte_at( row, offset + 3U ) );
            }
        }

        [[nodiscard]]
        grab::Result<ByteVector>
        make_filtered_scanlines( const grab::Image&  image,
                                 const EncodeLayout& layout )
        {
            ByteVector scanlines;
            scanlines.reserve( layout.filtered_bytes );

            for( std::uint32_t y = 0U; y < image.height; ++y )
            {
                const auto row = image.row( y );
                if( row.empty() )
                {
                    return invalid_image_error( "row is outside the pixel buffer" );
                }

                scanlines.push_back( static_cast<Byte>( FilterType::none ) );
                switch( image.format )
                {
                    case grab::PixelFormat::gray :
                    case grab::PixelFormat::rgb :
                    case grab::PixelFormat::rgba :
                    case grab::PixelFormat::rgb24 :
                        append_direct_row( scanlines, row, layout.png_row_bytes );
                        break;
                    case grab::PixelFormat::bgr :
                        append_bgr_row( scanlines, row, layout );
                        break;
                    case grab::PixelFormat::bgra :
                    case grab::PixelFormat::bgr0 :
                        append_bgra_row( scanlines, row, layout );
                        break;
                }
            }

            return scanlines;
        }

        [[nodiscard]]
        grab::Result<ByteVector>
        zlib_compress( const ByteVector& bytes )
        {
            if( bytes.size() >
                static_cast<std::size_t>( std::numeric_limits<uLong>::max() ) )
            {
                return codec_failure( "source buffer is too large for zlib" );
            }

            const auto source_size = static_cast<uLong>( bytes.size() );
            const auto bound       = compressBound( source_size );
            if( bound > std::numeric_limits<std::size_t>::max() )
            {
                return codec_failure( "compressed buffer bound overflows" );
            }

            ByteVector compressed( static_cast<std::size_t>( bound ) );
            auto       compressed_size = static_cast<uLongf>( bound );
            const auto status          = compress2( compressed.data(),
                                                    &compressed_size,
                                                    bytes.data(),
                                                    source_size,
                                                    Z_BEST_COMPRESSION );
            if( status != Z_OK )
            {
                return codec_failure( "zlib compression failed" );
            }

            compressed.resize( static_cast<std::size_t>( compressed_size ) );
            return compressed;
        }

        [[nodiscard]]
        grab::Result<ByteVector>
        zlib_inflate_exact( const ByteVector& compressed,
                            std::size_t       expected_size )
        {
            if( compressed.empty() )
            {
                return png_protocol_error( "missing IDAT data" );
            }
            if( compressed.size() >
                static_cast<std::size_t>( std::numeric_limits<uLong>::max() ) ||
                expected_size >
                static_cast<std::size_t>( std::numeric_limits<uLongf>::max() ) )
            {
                return png_protocol_error( "zlib buffer size is unsupported" );
            }

            ByteVector inflated( expected_size );
            auto       inflated_size = static_cast<uLongf>( expected_size );
            const auto status = uncompress( inflated.data(),
                                            &inflated_size,
                                            compressed.data(),
                                            static_cast<uLong>( compressed.size() ) );
            if( status != Z_OK || inflated_size != static_cast<uLongf>( expected_size ) )
            {
                return png_protocol_error( "IDAT zlib stream is invalid" );
            }

            return inflated;
        }

        [[nodiscard]]
        grab::Result<Ihdr>
        parse_ihdr( std::span<const Byte> data )
        {
            if( data.size() != kPngIhdrBytes )
            {
                return png_protocol_error( "IHDR has an invalid size" );
            }

            const auto width       = read_be32( data, kIhdrWidthOffset );
            const auto height      = read_be32( data, kIhdrHeightOffset );
            const auto bit_depth   = read_byte( data, kIhdrBitDepthOffset );
            const auto color_byte  = read_byte( data, kIhdrColorTypeOffset );
            const auto compression = read_byte( data, kIhdrCompressionOffset );
            const auto filter      = read_byte( data, kIhdrFilterOffset );
            const auto interlace   = read_byte( data, kIhdrInterlaceOffset );
            if( !width.has_value() ||
                !height.has_value() ||
                !bit_depth.has_value() ||
                !color_byte.has_value() ||
                !compression.has_value() ||
                !filter.has_value() ||
                !interlace.has_value() )
            {
                return png_protocol_error( "IHDR is truncated" );
            }

            const auto color_type = parse_color_type( *color_byte );
            if( !color_type.has_value() )
            {
                return png_protocol_error( "IHDR color type is unsupported" );
            }
            if( *width == 0U || *height == 0U )
            {
                return png_protocol_error( "IHDR dimensions must be non-zero" );
            }
            if( *bit_depth != kPngBitDepth8 )
            {
                return png_protocol_error( "only 8-bit PNG images are supported" );
            }
            if( *color_type == PngColorType::grayscale_alpha )
            {
                return png_protocol_error(
                    "grayscale alpha PNG images are unsupported"
                );
            }
            if( *compression !=
                kPngMethodNone ||
                *filter !=
                kPngMethodNone ||
                *interlace != kPngMethodNone )
            {
                return png_protocol_error(
                    "unsupported compression, filter, or interlace method"
                );
            }

            return Ihdr{
                .width       = *width,
                .height      = *height,
                .bit_depth   = *bit_depth,
                .color_type  = *color_type,
                .compression = *compression,
                .filter      = *filter,
                .interlace   = *interlace,
            };
        }

        [[nodiscard]]
        grab::Result<void>
        parse_plte( ParseState&           state,
                    std::span<const Byte> data )
        {
            if( !state.seen_ihdr || state.seen_idat )
            {
                return png_protocol_error( "PLTE appears in an invalid position" );
            }
            if( data.empty() ||
                data.size() %
                kPaletteEntryBytes !=
                0U ||
                data.size() /
                kPaletteEntryBytes > kMaxPaletteEntries )
            {
                return png_protocol_error( "PLTE has an invalid size" );
            }

            state.png.palette.assign( data.begin(), data.end() );
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        parse_trns( ParseState&           state,
                    std::span<const Byte> data )
        {
            if( !state.seen_ihdr || state.seen_idat )
            {
                return png_protocol_error( "tRNS appears in an invalid position" );
            }
            if( state.png.ihdr.color_type != PngColorType::palette )
            {
                return png_protocol_error( "tRNS is only supported for palette PNGs" );
            }
            if( state.png.palette.empty() )
            {
                return png_protocol_error( "palette tRNS appears before PLTE" );
            }

            const auto palette_entries = state.png.palette.size() / kPaletteEntryBytes;
            if( data.size() > palette_entries )
            {
                return png_protocol_error( "palette tRNS is larger than PLTE" );
            }

            state.png.transparency.assign( data.begin(), data.end() );
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        append_idat( ParseState&           state,
                     std::span<const Byte> data )
        {
            if( !state.seen_ihdr )
            {
                return png_protocol_error( "IDAT appears before IHDR" );
            }
            if( data.size() >
                kMaxCompressedBytes ||
                state.png.idat.size() >
                kMaxCompressedBytes -
                data.size() )
            {
                return png_protocol_error( "IDAT data is too large" );
            }

            state.png.idat.insert( state.png.idat.end(), data.begin(), data.end() );
            state.seen_idat = true;
            return {};
        }

        [[nodiscard]]
        grab::Result<bool>
        process_chunk( ParseState&           state,
                       const ChunkType&      type,
                       std::span<const Byte> data )
        {
            if( !is_chunk_type_valid( type ) )
            {
                return png_protocol_error( "chunk type contains non-letter bytes" );
            }

            if( type == kChunkIhdr )
            {
                if( state.seen_ihdr || state.seen_idat )
                {
                    return png_protocol_error( "IHDR appears in an invalid position" );
                }

                auto ihdr = parse_ihdr( data );
                if( !ihdr.has_value() )
                {
                    return std::unexpected( ihdr.error() );
                }
                state.png.ihdr  = *ihdr;
                state.seen_ihdr = true;
                return false;
            }

            if( !state.seen_ihdr )
            {
                return png_protocol_error( "IHDR must be the first chunk" );
            }

            if( type == kChunkPlte )
            {
                auto result = parse_plte( state, data );
                if( !result.has_value() )
                {
                    return std::unexpected( result.error() );
                }
                return false;
            }
            if( type == kChunkTrns )
            {
                auto result = parse_trns( state, data );
                if( !result.has_value() )
                {
                    return std::unexpected( result.error() );
                }
                return false;
            }
            if( type == kChunkIdat )
            {
                auto result = append_idat( state, data );
                if( !result.has_value() )
                {
                    return std::unexpected( result.error() );
                }
                return false;
            }
            if( type == kChunkIend )
            {
                if( !data.empty() )
                {
                    return png_protocol_error( "IEND is not empty" );
                }
                return true;
            }

            if( is_critical_chunk( type ) )
            {
                return png_protocol_error( "unknown critical chunk" );
            }
            return false;
        }

        [[nodiscard]]
        grab::Result<ParsedPng>
        parse_png( std::span<const Byte> bytes )
        {
            if( bytes.size() <
                kPngSignature.size() ||
                !std::ranges::equal( bytes.subspan( 0U, kPngSignature.size() ),
                                     kPngSignature ) )
            {
                return png_protocol_error( "signature mismatch" );
            }

            ParseState  state;
            std::size_t offset = kPngSignature.size();
            while( offset < bytes.size() )
            {
                if( !has_bytes( bytes, offset, kPngChunkOverheadBytes ) )
                {
                    return png_protocol_error( "truncated chunk header" );
                }

                const auto length = read_be32( bytes, offset );
                if( !length.has_value() )
                {
                    return png_protocol_error( "truncated chunk length" );
                }

                const auto data_size = static_cast<std::size_t>( *length );
                if( data_size > kMaxChunkBytes )
                {
                    return png_protocol_error( "chunk is too large" );
                }

                std::size_t chunk_bytes = 0U;
                if( !checked_add( kPngChunkOverheadBytes, data_size, chunk_bytes ) ||
                    !has_bytes( bytes, offset, chunk_bytes ) )
                {
                    return png_protocol_error( "truncated chunk data" );
                }

                const auto type_offset = offset + kPngChunkLengthBytes;
                const auto type        = read_chunk_type( bytes, type_offset );
                if( !type.has_value() )
                {
                    return png_protocol_error( "truncated chunk type" );
                }

                const auto data_offset  = type_offset + kPngChunkTypeBytes;
                const auto crc_offset   = data_offset + data_size;
                const auto expected_crc = read_be32( bytes, crc_offset );
                if( !expected_crc.has_value() )
                {
                    return png_protocol_error( "truncated chunk CRC" );
                }

                const auto data = bytes.subspan( data_offset, data_size );
                if( chunk_crc( *type, data ) != *expected_crc )
                {
                    return png_protocol_error( "chunk CRC mismatch" );
                }

                auto done = process_chunk( state, *type, data );
                if( !done.has_value() )
                {
                    return std::unexpected( done.error() );
                }

                offset += chunk_bytes;
                if( *done )
                {
                    if( offset != bytes.size() )
                    {
                        return png_protocol_error( "trailing data after IEND" );
                    }
                    if( !state.seen_idat )
                    {
                        return png_protocol_error( "missing IDAT data" );
                    }
                    if( state.png.ihdr.color_type ==
                        PngColorType::palette &&
                        state.png.palette.empty() )
                    {
                        return png_protocol_error( "palette PNG is missing PLTE" );
                    }
                    return std::move( state.png );
                }
            }

            return png_protocol_error( "missing IEND chunk" );
        }

        [[nodiscard]]
        std::optional<std::size_t>
        source_channels( PngColorType color_type ) noexcept
        {
            switch( color_type )
            {
                case PngColorType::grayscale :
                case PngColorType::palette :
                    return kGrayBytes;
                case PngColorType::truecolor :
                    return kRgbBytes;
                case PngColorType::truecolor_alpha :
                    return kRgbaBytes;
                case PngColorType::grayscale_alpha :
                    return std::nullopt;
            }

            return std::nullopt;
        }

        [[nodiscard]]
        grab::Result<DecodeLayout>
        decode_layout_for( const ParsedPng& png )
        {
            const auto input_channels = source_channels( png.ihdr.color_type );
            if( !input_channels.has_value() )
            {
                return png_protocol_error( "unsupported color type" );
            }

            const auto   width  = static_cast<std::size_t>( png.ihdr.width );
            const auto   height = static_cast<std::size_t>( png.ihdr.height );
            DecodeLayout layout{
                .source_row_bytes   = 0U,
                .filtered_row_bytes = 0U,
                .filtered_bytes     = 0U,
                .filter_pixel_bytes = *input_channels,
                .output_stride      = 0U,
                .output_bytes       = 0U,
                .output_format      = grab::PixelFormat::rgba,
            };
            if( !checked_mul( width, *input_channels, layout.source_row_bytes ) ||
                !checked_add( layout.source_row_bytes, 1U, layout.filtered_row_bytes ) ||
                !checked_mul( layout.filtered_row_bytes,
                              height,
                              layout.filtered_bytes ) )
            {
                return png_protocol_error( "decoded source size overflows" );
            }

            std::size_t output_channels = 0U;
            switch( png.ihdr.color_type )
            {
                case PngColorType::grayscale :
                    layout.output_format = grab::PixelFormat::gray;
                    output_channels      = kGrayBytes;
                    break;
                case PngColorType::truecolor :
                    layout.output_format = grab::PixelFormat::rgb;
                    output_channels      = kRgbBytes;
                    break;
                case PngColorType::truecolor_alpha :
                    layout.output_format = grab::PixelFormat::rgba;
                    output_channels      = kRgbaBytes;
                    break;
                case PngColorType::palette :
                    layout.output_format = png.transparency.empty()
                                             ? grab::PixelFormat::rgb
                                             : grab::PixelFormat::rgba;
                    output_channels = png.transparency.empty() ? kRgbBytes : kRgbaBytes;
                    break;
                case PngColorType::grayscale_alpha :
                    return png_protocol_error( "unsupported color type" );
            }

            if( !checked_mul( width, output_channels, layout.output_stride ) ||
                !checked_mul( height, layout.output_stride, layout.output_bytes ) )
            {
                return png_protocol_error( "decoded output size overflows" );
            }

            if( layout.output_stride >
                static_cast<std::size_t>( std::numeric_limits<std::uint32_t>::max() ) ||
                layout.filtered_bytes >
                kMaxDecodedBytes ||
                layout.output_bytes > kMaxDecodedBytes )
            {
                return png_protocol_error( "decoded image is too large" );
            }

            return layout;
        }

        [[nodiscard]]
        Byte
        paeth_predictor( Byte left,
                         Byte up,
                         Byte up_left ) noexcept
        {
            const int left_value    = left;
            const int up_value      = up;
            const int up_left_value = up_left;
            const int estimate      = left_value + up_value - up_left_value;
            const int left_distance = std::abs( estimate - left_value );
            const int up_distance   = std::abs( estimate - up_value );
            const int diag_distance = std::abs( estimate - up_left_value );

            if( left_distance <= up_distance && left_distance <= diag_distance )
            {
                return left;
            }
            if( up_distance <= diag_distance )
            {
                return up;
            }
            return up_left;
        }

        [[nodiscard]]
        Byte
        filter_predictor( FilterType filter,
                          Byte       left,
                          Byte       up,
                          Byte       up_left ) noexcept
        {
            switch( filter )
            {
                case FilterType::none :
                    return 0U;
                case FilterType::sub :
                    return left;
                case FilterType::up :
                    return up;
                case FilterType::average :
                    return static_cast<Byte>( ( static_cast<unsigned int>( left ) +
                                                static_cast<unsigned int>( up ) ) /
                                              2U );
                case FilterType::paeth :
                    return paeth_predictor( left, up, up_left );
            }

            return 0U;
        }

        [[nodiscard]]
        Byte
        reconstruct_byte( Byte filtered,
                          Byte predictor ) noexcept
        {
            return static_cast<Byte>( ( static_cast<unsigned int>( filtered ) +
                                        static_cast<unsigned int>( predictor ) ) %
                                      kByteModulo );
        }

        [[nodiscard]]
        grab::Result<ByteVector>
        unfilter_scanlines( const ByteVector&   inflated,
                            const DecodeLayout& layout,
                            std::uint32_t       height )
        {
            if( inflated.size() != layout.filtered_bytes )
            {
                return png_protocol_error( "inflated data has the wrong size" );
            }

            ByteVector raw( layout.source_row_bytes *
                            static_cast<std::size_t>( height ) );
            for( std::size_t y = 0U; y < static_cast<std::size_t>( height ); ++y )
            {
                const auto input_row  = y * layout.filtered_row_bytes;
                const auto output_row = y * layout.source_row_bytes;
                const auto filter     = parse_filter_type( inflated.at( input_row ) );
                if( !filter.has_value() )
                {
                    return png_protocol_error( "unknown scanline filter" );
                }

                for( std::size_t x = 0U; x < layout.source_row_bytes; ++x )
                {
                    const Byte left =
                        x >= layout.filter_pixel_bytes
                            ? raw.at( output_row + x - layout.filter_pixel_bytes )
                            : Byte{ 0U };
                    const Byte up =
                        y > 0U ? raw.at( output_row - layout.source_row_bytes + x )
                               : Byte{ 0U };
                    const Byte up_left = y > 0U && x >= layout.filter_pixel_bytes
                                           ? raw.at( output_row -
                                                     layout.source_row_bytes +
                                                     x -
                                                     layout.filter_pixel_bytes )
                                           : Byte{ 0U };
                    const auto predictor =
                        filter_predictor( *filter, left, up, up_left );
                    raw.at( output_row + x ) =
                        reconstruct_byte( inflated.at( input_row + 1U + x ), predictor );
                }
            }

            return raw;
        }

        [[nodiscard]]
        grab::Result<ByteVector>
        expand_palette( const ParsedPng&    png,
                        const DecodeLayout& layout,
                        const ByteVector&   raw )
        {
            const auto palette_entries = png.palette.size() / kPaletteEntryBytes;
            ByteVector expanded;
            expanded.reserve( layout.output_bytes );

            for( const auto index_byte : raw )
            {
                const auto index = static_cast<std::size_t>( index_byte );
                if( index >= palette_entries )
                {
                    return png_protocol_error( "palette index is outside PLTE" );
                }

                const auto palette_offset = index * kPaletteEntryBytes;
                expanded.push_back( png.palette.at( palette_offset ) );
                expanded.push_back( png.palette.at( palette_offset + 1U ) );
                expanded.push_back( png.palette.at( palette_offset + 2U ) );
                if( !png.transparency.empty() )
                {
                    const auto alpha = index < png.transparency.size()
                                         ? png.transparency.at( index )
                                         : kOpaqueAlpha;
                    expanded.push_back( alpha );
                }
            }

            if( expanded.size() != layout.output_bytes )
            {
                return png_protocol_error( "expanded palette has the wrong size" );
            }
            return expanded;
        }

        [[nodiscard]]
        grab::Result<grab::Image>
        image_from_raw( const ParsedPng&    png,
                        const DecodeLayout& layout,
                        const ByteVector&   raw )
        {
            ByteVector pixels = raw;
            if( png.ihdr.color_type == PngColorType::palette )
            {
                auto expanded = expand_palette( png, layout, raw );
                if( !expanded.has_value() )
                {
                    return std::unexpected( expanded.error() );
                }
                pixels = std::move( *expanded );
            }

            return grab::Image{
                .width  = png.ihdr.width,
                .height = png.ihdr.height,
                .stride = static_cast<std::uint32_t>( layout.output_stride ),
                .format = layout.output_format,
                .pixels = bytes_from_u8( pixels ),
            };
        }

    }    // namespace

    grab::Result<std::vector<std::byte>>
    encode_png( const grab::Image& image )
    {
        auto layout = validate_encode_image( image );
        if( !layout.has_value() )
        {
            return std::unexpected( layout.error() );
        }

        auto scanlines = make_filtered_scanlines( image, *layout );
        if( !scanlines.has_value() )
        {
            return std::unexpected( scanlines.error() );
        }

        auto compressed = zlib_compress( *scanlines );
        if( !compressed.has_value() )
        {
            return std::unexpected( compressed.error() );
        }

        ByteVector png;
        png.reserve( kPngSignature.size() +
                     kPngChunkOverheadBytes +
                     kPngIhdrBytes +
                     compressed->size() +
                     ( kPngChunkOverheadBytes * 2U ) );
        png.insert( png.end(), kPngSignature.begin(), kPngSignature.end() );

        ByteVector ihdr;
        ihdr.reserve( kPngIhdrBytes );
        append_be32( ihdr, image.width );
        append_be32( ihdr, image.height );
        ihdr.push_back( kPngBitDepth8 );
        ihdr.push_back( static_cast<Byte>( layout->color_type ) );
        ihdr.push_back( kPngMethodNone );
        ihdr.push_back( kPngMethodNone );
        ihdr.push_back( kPngMethodNone );

        auto ihdr_chunk = append_chunk( png, kChunkIhdr, ihdr );
        if( !ihdr_chunk.has_value() )
        {
            return std::unexpected( ihdr_chunk.error() );
        }
        auto idat_chunk = append_chunk( png, kChunkIdat, *compressed );
        if( !idat_chunk.has_value() )
        {
            return std::unexpected( idat_chunk.error() );
        }
        auto iend_chunk = append_chunk( png, kChunkIend, std::span<const Byte>{} );
        if( !iend_chunk.has_value() )
        {
            return std::unexpected( iend_chunk.error() );
        }

        return bytes_from_u8( png );
    }

    grab::Result<grab::Image>
    decode_png( std::span<const std::byte> bytes )
    {
        const auto input  = bytes_to_u8( bytes );
        auto       parsed = parse_png( input );
        if( !parsed.has_value() )
        {
            return std::unexpected( parsed.error() );
        }

        auto layout = decode_layout_for( *parsed );
        if( !layout.has_value() )
        {
            return std::unexpected( layout.error() );
        }

        auto inflated = zlib_inflate_exact( parsed->idat, layout->filtered_bytes );
        if( !inflated.has_value() )
        {
            return std::unexpected( inflated.error() );
        }

        auto raw = unfilter_scanlines( *inflated, *layout, parsed->ihdr.height );
        if( !raw.has_value() )
        {
            return std::unexpected( raw.error() );
        }

        return image_from_raw( *parsed, *layout, *raw );
    }

}    // namespace grab::codec
