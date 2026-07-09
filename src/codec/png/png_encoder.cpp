#include "codec/png/png_encoder.hpp"
#include "core/checked.hpp"
#include "core/pixel_traits.hpp"
#include "grab/image.hpp"
#include "grab/result.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>
#include <zconf.h>
#include <zlib.h>

namespace grab::codec
{

    namespace
    {

        constexpr std::uint32_t empty_dimension        = 0U;
        constexpr std::size_t   filter_mode_chunk_size = 4'096U;
        constexpr std::size_t   png_signature_size     = 8U;
        constexpr std::size_t   png_chunk_length_size  = 4U;
        constexpr std::size_t   png_chunk_tag_size     = 4U;
        constexpr std::size_t   png_chunk_crc_size     = 4U;
        constexpr std::size_t   png_chunk_overhead =
            png_chunk_length_size + png_chunk_tag_size + png_chunk_crc_size;
        constexpr std::size_t png_bytes_per_pixel =
            grab::PixelTraits<grab::PixelFormat::rgb24>::bytes_per_pixel;
        constexpr std::size_t   ihdr_payload_size            = 13U;
        constexpr std::size_t   phys_payload_size            = 9U;
        constexpr std::size_t   row_filter_byte_count        = 1U;
        constexpr std::size_t   first_offset                 = 0U;
        constexpr std::size_t   single_item                  = 1U;
        constexpr std::uint8_t  png_filter_none              = 0X00U;
        constexpr std::uint8_t  png_bit_depth                = 8U;
        constexpr std::uint8_t  png_color_type_truecolor_rgb = 2U;
        constexpr std::uint8_t  png_compression_deflate      = 0U;
        constexpr std::uint8_t  png_filter_method_adaptive   = 0U;
        constexpr std::uint8_t  png_interlace_none           = 0U;
        constexpr std::uint32_t phys_x_pixels_per_unit       = 0U;
        constexpr std::uint32_t phys_y_pixels_per_unit       = 1U;
        constexpr std::uint8_t  phys_unit_unknown            = 0U;
        constexpr std::uint32_t big_endian_byte_mask         = 0XFFU;
        constexpr std::uint32_t big_endian_first_byte_shift  = 24U;
        constexpr std::uint32_t big_endian_second_byte_shift = 16U;
        constexpr std::uint32_t big_endian_third_byte_shift  = 8U;
        constexpr int           zlib_compression_level       = Z_DEFAULT_COMPRESSION;
        constexpr int           zlib_ok                      = Z_OK;
        constexpr int           zlib_stream_end              = Z_STREAM_END;
        constexpr int           zlib_finish                  = Z_FINISH;
        constexpr uLong         initial_crc                  = 0U;
        constexpr uInt          no_crc_input_bytes           = 0U;

        [[nodiscard]]
        constexpr std::uint8_t
        ascii_byte( char value ) noexcept
        {
            return static_cast<std::uint8_t>( value );
        }

        constexpr std::array<std::uint8_t, png_signature_size> png_signature{
            0X89U,
            ascii_byte( 'P' ),
            ascii_byte( 'N' ),
            ascii_byte( 'G' ),
            0X0DU,
            0X0AU,
            0X1AU,
            0X0AU,
        };
        constexpr std::array<std::uint8_t, png_chunk_tag_size> ihdr_tag{
            ascii_byte( 'I' ),
            ascii_byte( 'H' ),
            ascii_byte( 'D' ),
            ascii_byte( 'R' ),
        };
        constexpr std::array<std::uint8_t, png_chunk_tag_size> phys_tag{
            ascii_byte( 'p' ),
            ascii_byte( 'H' ),
            ascii_byte( 'Y' ),
            ascii_byte( 's' ),
        };
        constexpr std::array<std::uint8_t, png_chunk_tag_size> idat_tag{
            ascii_byte( 'I' ),
            ascii_byte( 'D' ),
            ascii_byte( 'A' ),
            ascii_byte( 'T' ),
        };
        constexpr std::array<std::uint8_t, png_chunk_tag_size> iend_tag{
            ascii_byte( 'I' ),
            ascii_byte( 'E' ),
            ascii_byte( 'N' ),
            ascii_byte( 'D' ),
        };

        class ZlibDeflater
        {
            public:

                ZlibDeflater()                      = default;
                ZlibDeflater( const ZlibDeflater& ) = delete;
                ZlibDeflater( ZlibDeflater&& )      = delete;
                ZlibDeflater&
                operator=( const ZlibDeflater& ) = delete;
                ZlibDeflater&
                operator=( ZlibDeflater&& ) = delete;

                ~ZlibDeflater()
                {
                    if( initialized )
                    {
                        static_cast<void>( deflateEnd( &zlib_stream ) );
                    }
                }

                [[nodiscard]]
                grab::Result<void>
                init()
                {
                    const int status =
                        deflateInit( &zlib_stream, zlib_compression_level );
                    if( status != zlib_ok )
                    {
                        return grab::fail( grab::ErrorCode::internal_fault,
                                           "zlib deflate initialization failed" );
                    }
                    initialized = true;
                    return {};
                }

                [[nodiscard]]
                z_stream&
                stream() noexcept
                {
                    return zlib_stream;
                }

            private:

                z_stream zlib_stream{};
                bool     initialized = false;
        };

        [[nodiscard]]
        grab::Result<uInt>
        checked_zlib_size( std::size_t value )
        {
            return grab::checked_cast<uInt>( value,
                                             grab::ErrorCode::invalid_argument,
                                             "png input is too large for zlib" );
        }

        [[nodiscard]]
        grab::Result<uLong>
        checked_zlib_long_size( std::size_t value )
        {
            return grab::checked_cast<uLong>( value,
                                              grab::ErrorCode::invalid_argument,
                                              "png input is too large for zlib" );
        }

        [[nodiscard]]
        grab::Result<std::size_t>
        checked_vector_size( uLong value )
        {
            return grab::checked_cast<std::size_t>(
                value,
                grab::ErrorCode::invalid_argument,
                "png compressed stream is too large"
            );
        }

        void
        append_bytes( std::vector<std::uint8_t>&    output,
                      std::span<const std::uint8_t> bytes )
        {
            output.insert( output.end(), bytes.begin(), bytes.end() );
        }

        void
        append_big_endian_u32( std::vector<std::uint8_t>& output,
                               std::uint32_t              value )
        {
            output.push_back( static_cast<std::uint8_t>(
                ( value >> big_endian_first_byte_shift ) & big_endian_byte_mask
            ) );
            output.push_back( static_cast<std::uint8_t>(
                ( value >> big_endian_second_byte_shift ) & big_endian_byte_mask
            ) );
            output.push_back( static_cast<std::uint8_t>(
                ( value >> big_endian_third_byte_shift ) & big_endian_byte_mask
            ) );
            output.push_back( static_cast<std::uint8_t>( value &
                                                         big_endian_byte_mask ) );
        }

        template<grab::PixelFormat Format>
        void
        append_png_row( std::vector<std::uint8_t>&    output,
                        std::span<const std::uint8_t> row )
        {
            using Traits = grab::PixelTraits<Format>;

            for( std::size_t offset  = first_offset; offset < row.size();
                 offset             += Traits::bytes_per_pixel )
            {
                const auto pixel = row.subspan( offset, Traits::bytes_per_pixel );
                output.push_back( Traits::red( pixel ) );
                output.push_back( Traits::green( pixel ) );
                output.push_back( Traits::blue( pixel ) );
            }
        }

        template<grab::PixelFormat Format>
        [[nodiscard]]
        grab::Result<std::vector<std::uint8_t>>
        build_scanline_data_for_format( const grab::ImageView& image )
        {
            using Traits = grab::PixelTraits<Format>;
            const std::size_t row_input_size =
                static_cast<std::size_t>( image.width ) * Traits::bytes_per_pixel;
            if( image.stride < row_input_size )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   "png image stride is too small" );
            }

            const std::size_t row_output_size =
                row_filter_byte_count +
                ( static_cast<std::size_t>( image.width ) * png_bytes_per_pixel );
            const std::size_t output_size =
                static_cast<std::size_t>( image.height ) * row_output_size;
            const std::size_t last_row_offset =
                ( static_cast<std::size_t>( image.height ) - single_item ) *
                image.stride;
            const std::size_t source_size = last_row_offset + row_input_size;

            const std::span<const std::uint8_t> source( image.data, source_size );
            std::vector<std::uint8_t>           output;
            output.reserve( output_size );

            for( std::size_t row_index = first_offset;
                 row_index < static_cast<std::size_t>( image.height );
                 ++row_index )
            {
                output.push_back( png_filter_none );
                const std::size_t row_offset = row_index * image.stride;
                const auto        row = source.subspan( row_offset, row_input_size );
                append_png_row<Format>( output, row );
            }

            return output;
        }

        using ScanlineBuilder =
            grab::Result<std::vector<std::uint8_t>> ( * )( const grab::ImageView& );

        struct ScanlineDispatch
        {
                grab::PixelFormat format = grab::PixelFormat::rgb24;
                ScanlineBuilder   build  = nullptr;
        };

        constexpr std::size_t scanline_dispatch_count = 2U;
        constexpr std::array<ScanlineDispatch, scanline_dispatch_count>
            scanline_dispatch{
                ScanlineDispatch{
                                 .format = grab::PixelFormat::rgb24,
                                 .build  = &build_scanline_data_for_format<grab::PixelFormat::rgb24>,
                                 },
                ScanlineDispatch{
                                 .format = grab::PixelFormat::bgr0,
                                 .build  = &build_scanline_data_for_format<grab::PixelFormat::bgr0>,
                                 },
        };

        [[nodiscard]]
        grab::Result<std::vector<std::uint8_t>>
        build_scanline_data( const grab::ImageView& image )
        {
            if( image.width == empty_dimension || image.height == empty_dimension )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   "png image dimensions must be non-zero" );
            }
            if( image.data == nullptr )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   "png image data is null" );
            }

            for( const ScanlineDispatch& dispatch : scanline_dispatch )
            {
                if( dispatch.format == image.format && dispatch.build != nullptr )
                {
                    return dispatch.build( image );
                }
            }
            return grab::fail( grab::ErrorCode::invalid_argument,
                               "unsupported png pixel format" );
        }

        [[nodiscard]]
        grab::Result<std::vector<std::uint8_t>>
        deflate_scanlines( std::vector<std::uint8_t>& scanlines )
        {
            ZlibDeflater deflater;
            if( auto init = deflater.init(); !init.has_value() )
            {
                return std::unexpected( init.error() );
            }

            auto input_size = checked_zlib_size( scanlines.size() );
            if( !input_size.has_value() )
            {
                return std::unexpected( input_size.error() );
            }

            auto source_size = checked_zlib_long_size( scanlines.size() );
            if( !source_size.has_value() )
            {
                return std::unexpected( source_size.error() );
            }

            const uLong deflate_bound = deflateBound( &deflater.stream(), *source_size );
            auto        output_size   = checked_vector_size( deflate_bound );
            if( !output_size.has_value() )
            {
                return std::unexpected( output_size.error() );
            }

            std::vector<std::uint8_t> output( *output_size );
            auto output_zlib_size = checked_zlib_size( output.size() );
            if( !output_zlib_size.has_value() )
            {
                return std::unexpected( output_zlib_size.error() );
            }

            auto& stream     = deflater.stream();
            stream.next_in   = scanlines.data();
            stream.avail_in  = *input_size;
            stream.next_out  = output.data();
            stream.avail_out = *output_zlib_size;

            const int status = deflate( &stream, zlib_finish );
            if( status != zlib_stream_end )
            {
                return grab::fail( grab::ErrorCode::internal_fault,
                                   "zlib deflate failed" );
            }

            output.resize( static_cast<std::size_t>( stream.total_out ) );
            return output;
        }

        void
        append_chunk( std::vector<std::uint8_t>&            png,
                      const std::array<std::uint8_t,
                                       png_chunk_tag_size>& tag,
                      std::span<const std::uint8_t>         payload )
        {
            append_big_endian_u32( png, static_cast<std::uint32_t>( payload.size() ) );
            append_bytes( png, tag );
            append_bytes( png, payload );

            uLong crc = crc32( initial_crc, nullptr, no_crc_input_bytes );
            crc       = crc32( crc, tag.data(), static_cast<uInt>( tag.size() ) );
            if( !payload.empty() )
            {
                crc = crc32( crc, payload.data(), static_cast<uInt>( payload.size() ) );
            }
            append_big_endian_u32( png, static_cast<std::uint32_t>( crc ) );
        }

        [[nodiscard]]
        std::vector<std::uint8_t>
        make_ihdr_payload( std::uint32_t width,
                           std::uint32_t height )
        {
            std::vector<std::uint8_t> payload;
            payload.reserve( ihdr_payload_size );
            append_big_endian_u32( payload, width );
            append_big_endian_u32( payload, height );
            payload.push_back( png_bit_depth );
            payload.push_back( png_color_type_truecolor_rgb );
            payload.push_back( png_compression_deflate );
            payload.push_back( png_filter_method_adaptive );
            payload.push_back( png_interlace_none );
            return payload;
        }

        [[nodiscard]]
        std::vector<std::uint8_t>
        make_phys_payload()
        {
            std::vector<std::uint8_t> payload;
            payload.reserve( phys_payload_size );
            append_big_endian_u32( payload, phys_x_pixels_per_unit );
            append_big_endian_u32( payload, phys_y_pixels_per_unit );
            payload.push_back( phys_unit_unknown );
            return payload;
        }

        [[nodiscard]]
        std::size_t
        idat_chunk_count( std::size_t compressed_size ) noexcept
        {
            return ( compressed_size + filter_mode_chunk_size - single_item ) /
                   filter_mode_chunk_size;
        }

        [[nodiscard]]
        std::size_t
        png_reserve_size( std::size_t compressed_size ) noexcept
        {
            constexpr std::size_t fixed_chunk_count = 3U;
            return png_signature_size +
                   ( fixed_chunk_count * png_chunk_overhead ) +
                   ihdr_payload_size +
                   phys_payload_size +
                   compressed_size +
                   ( idat_chunk_count( compressed_size ) * png_chunk_overhead );
        }

        [[nodiscard]]
        std::vector<std::uint8_t>
        assemble_png( std::uint32_t                 width,
                      std::uint32_t                 height,
                      std::span<const std::uint8_t> compressed )
        {
            std::vector<std::uint8_t> png;
            png.reserve( png_reserve_size( compressed.size() ) );
            append_bytes( png, png_signature );

            const auto ihdr_payload = make_ihdr_payload( width, height );
            append_chunk( png, ihdr_tag, ihdr_payload );

            const auto phys_payload = make_phys_payload();
            append_chunk( png, phys_tag, phys_payload );

            for( std::size_t offset  = first_offset; offset < compressed.size();
                 offset             += filter_mode_chunk_size )
            {
                const std::size_t chunk_size =
                    std::min( filter_mode_chunk_size, compressed.size() - offset );
                append_chunk( png, idat_tag, compressed.subspan( offset, chunk_size ) );
            }

            append_chunk( png, iend_tag, std::span<const std::uint8_t>{} );
            return png;
        }

    }    // namespace

    grab::Result<std::vector<std::uint8_t>>
    encode_png( const grab::ImageView& image )
    {
        auto scanlines = build_scanline_data( image );
        if( !scanlines.has_value() )
        {
            return std::unexpected( scanlines.error() );
        }

        auto compressed = deflate_scanlines( *scanlines );
        if( !compressed.has_value() )
        {
            return std::unexpected( compressed.error() );
        }

        return assemble_png( image.width, image.height, *compressed );
    }

}    // namespace grab::codec
