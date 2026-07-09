#include "grab/image.hpp"
#include "grab/result.hpp"
#include "image/compare.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace grab::image
{
    namespace
    {

        constexpr std::size_t      kRgbaBytes       = 4U;
        constexpr std::size_t      kRedOffset       = 0U;
        constexpr std::size_t      kGreenOffset     = 1U;
        constexpr std::size_t      kBlueOffset      = 2U;
        constexpr std::size_t      kAlphaOffset     = 3U;
        constexpr std::size_t      kSingleByte      = 1U;
        constexpr unsigned int     kRgbChannelCount = 3U;
        constexpr unsigned int     kDimDivisor      = 2U;
        constexpr std::uint8_t     kOpaqueAlpha     = 0XFFU;
        constexpr std::uint8_t     kHighlightRed    = 0XFFU;
        constexpr std::uint8_t     kHighlightGreen  = 0U;
        constexpr std::uint8_t     kHighlightBlue   = 0U;
        constexpr std::string_view kInvalidPrefix   = "image comparison: ";

        struct Layout
        {
                std::size_t   bytes_per_pixel = 0U;
                std::size_t   row_bytes       = 0U;
                std::uint64_t total_pixels    = 0U;
        };

        struct Rgb
        {
                unsigned int red   = 0U;
                unsigned int green = 0U;
                unsigned int blue  = 0U;
        };

        [[nodiscard]]
        std::unexpected<grab::Error>
        invalid_argument_error( std::string message )
        {
            return grab::fail( grab::ErrorCode::invalid_argument,
                               std::string{ kInvalidPrefix } + std::move( message ) );
        }

        [[nodiscard]]
        bool
        multiply_overflows_size( std::size_t left,
                                 std::size_t right ) noexcept
        {
            return right !=
                   0U &&
                   left > ( std::numeric_limits<std::size_t>::max() / right );
        }

        [[nodiscard]]
        bool
        add_overflows_size( std::size_t left,
                            std::size_t right ) noexcept
        {
            return left > ( std::numeric_limits<std::size_t>::max() - right );
        }

        [[nodiscard]]
        std::uint64_t
        pixel_count( const grab::Image& image ) noexcept
        {
            return static_cast<std::uint64_t>( image.width ) *
                   static_cast<std::uint64_t>( image.height );
        }

        [[nodiscard]]
        grab::Result<Layout>
        validate_image_layout( const grab::Image& image )
        {
            const auto pixel_bytes =
                static_cast<std::size_t>( grab::bytes_per_pixel( image.format ) );
            const auto width = static_cast<std::size_t>( image.width );
            if( multiply_overflows_size( width, pixel_bytes ) )
            {
                return invalid_argument_error( "row byte count overflows size_t" );
            }

            const auto row_bytes = width * pixel_bytes;
            if( image.height !=
                0U &&
                static_cast<std::size_t>( image.stride ) < row_bytes )
            {
                return invalid_argument_error( "stride is smaller than the pixel row" );
            }

            if( image.height != 0U )
            {
                const auto last_row_index =
                    static_cast<std::size_t>( image.height - 1U );
                const auto stride = static_cast<std::size_t>( image.stride );
                if( multiply_overflows_size( last_row_index, stride ) )
                {
                    return invalid_argument_error( "row offset overflows size_t" );
                }

                const auto last_row_offset = last_row_index * stride;
                if( add_overflows_size( last_row_offset, row_bytes ) )
                {
                    return invalid_argument_error(
                        "pixel buffer size overflows size_t"
                    );
                }

                const auto required_size = last_row_offset + row_bytes;
                if( image.pixels.size() < required_size )
                {
                    return invalid_argument_error(
                        "pixel buffer is shorter than image rows"
                    );
                }
            }

            return Layout{
                .bytes_per_pixel = pixel_bytes,
                .row_bytes       = row_bytes,
                .total_pixels    = pixel_count( image ),
            };
        }

        [[nodiscard]]
        grab::Result<Layout>
        validate_pair( const grab::Image& a,
                       const grab::Image& b )
        {
            if( a.width != b.width || a.height != b.height )
            {
                return invalid_argument_error( "image dimensions differ" );
            }

            if( a.format != b.format )
            {
                return invalid_argument_error( "image formats differ" );
            }

            auto a_layout = validate_image_layout( a );
            if( !a_layout.has_value() )
            {
                return std::unexpected( a_layout.error() );
            }

            auto b_layout = validate_image_layout( b );
            if( !b_layout.has_value() )
            {
                return std::unexpected( b_layout.error() );
            }

            return *a_layout;
        }

        [[nodiscard]]
        std::span<const std::byte>
        row_pixels( const grab::Image& image,
                    std::uint32_t      y,
                    std::size_t        row_bytes )
        {
            const std::span<const std::byte> bytes{ image.pixels };
            const auto                       offset =
                static_cast<std::size_t>( y ) * static_cast<std::size_t>( image.stride );
            return bytes.subspan( offset, row_bytes );
        }

        [[nodiscard]]
        bool
        channel_differs( std::byte    left,
                         std::byte    right,
                         std::uint8_t tolerance ) noexcept
        {
            const auto left_value  = std::to_integer<unsigned int>( left );
            const auto right_value = std::to_integer<unsigned int>( right );
            const auto delta       = left_value > right_value ? left_value - right_value
                                                              : right_value - left_value;
            return delta > static_cast<unsigned int>( tolerance );
        }

        [[nodiscard]]
        std::byte
        byte_at( std::span<const std::byte> bytes,
                 std::size_t                offset )
        {
            return bytes.subspan( offset, kSingleByte ).front();
        }

        [[nodiscard]]
        bool
        pixel_differs( std::span<const std::byte> left,
                       std::span<const std::byte> right,
                       std::size_t                offset,
                       std::size_t                bytes_per_pixel,
                       std::uint8_t               tolerance )
        {
            const auto left_pixel  = left.subspan( offset, bytes_per_pixel );
            const auto right_pixel = right.subspan( offset, bytes_per_pixel );
            for( std::size_t channel = 0U; channel < bytes_per_pixel; ++channel )
            {
                if( channel_differs( byte_at( left_pixel, channel ),
                                     byte_at( right_pixel, channel ),
                                     tolerance ) )
                {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]]
        grab::Result<Rect>
        rect_from_bounds( std::uint32_t min_x,
                          std::uint32_t min_y,
                          std::uint32_t max_x,
                          std::uint32_t max_y )
        {
            constexpr auto kMaxRectOrigin =
                static_cast<std::uint32_t>( std::numeric_limits<std::int32_t>::max() );
            if( min_x > kMaxRectOrigin || min_y > kMaxRectOrigin )
            {
                return invalid_argument_error( "diff rectangle origin exceeds int32_t" );
            }

            return Rect{
                .x      = static_cast<std::int32_t>( min_x ),
                .y      = static_cast<std::int32_t>( min_y ),
                .width  = ( max_x - min_x ) + 1U,
                .height = ( max_y - min_y ) + 1U,
            };
        }

        [[nodiscard]]
        std::uint32_t
        output_stride( std::uint32_t width )
        {
            constexpr auto kMaxStride = std::numeric_limits<std::uint32_t>::max();
            if( width > kMaxStride / static_cast<std::uint32_t>( kRgbaBytes ) )
            {
                return 0U;
            }
            return width * static_cast<std::uint32_t>( kRgbaBytes );
        }

        [[nodiscard]]
        grab::Result<std::size_t>
        output_pixel_bytes( std::uint32_t width,
                            std::uint32_t height )
        {
            const auto stride = output_stride( width );
            if( width != 0U && stride == 0U )
            {
                return invalid_argument_error( "RGBA output stride overflows uint32_t" );
            }

            const auto stride_size = static_cast<std::size_t>( stride );
            const auto height_size = static_cast<std::size_t>( height );
            if( multiply_overflows_size( stride_size, height_size ) )
            {
                return invalid_argument_error( "RGBA output size overflows size_t" );
            }

            return stride_size * height_size;
        }

        [[nodiscard]]
        std::byte
        byte_from( std::uint8_t value ) noexcept
        {
            return static_cast<std::byte>( value );
        }

        [[nodiscard]]
        Rgb
        source_rgb( std::span<const std::byte> row,
                    std::size_t                offset,
                    grab::PixelFormat          format )
        {
            switch( format )
            {
                case grab::PixelFormat::rgba :
                    return Rgb{
                        .red = std::to_integer<unsigned int>(
                            byte_at( row, offset + kRedOffset )
                        ),
                        .green = std::to_integer<unsigned int>(
                            byte_at( row, offset + kGreenOffset )
                        ),
                        .blue = std::to_integer<unsigned int>(
                            byte_at( row, offset + kBlueOffset )
                        ),
                    };
                case grab::PixelFormat::bgr0 :
                case grab::PixelFormat::bgra :
                    return Rgb{
                        .red = std::to_integer<unsigned int>(
                            byte_at( row, offset + kBlueOffset )
                        ),
                        .green = std::to_integer<unsigned int>(
                            byte_at( row, offset + kGreenOffset )
                        ),
                        .blue = std::to_integer<unsigned int>(
                            byte_at( row, offset + kRedOffset )
                        ),
                    };
                case grab::PixelFormat::rgb24 :
                case grab::PixelFormat::rgb :
                    return Rgb{
                        .red = std::to_integer<unsigned int>(
                            byte_at( row, offset + kRedOffset )
                        ),
                        .green = std::to_integer<unsigned int>(
                            byte_at( row, offset + kGreenOffset )
                        ),
                        .blue = std::to_integer<unsigned int>(
                            byte_at( row, offset + kBlueOffset )
                        ),
                    };
                case grab::PixelFormat::bgr :
                    return Rgb{
                        .red = std::to_integer<unsigned int>(
                            byte_at( row, offset + kBlueOffset )
                        ),
                        .green = std::to_integer<unsigned int>(
                            byte_at( row, offset + kGreenOffset )
                        ),
                        .blue = std::to_integer<unsigned int>(
                            byte_at( row, offset + kRedOffset )
                        ),
                    };
                case grab::PixelFormat::gray :
                    {
                        const auto gray =
                            std::to_integer<unsigned int>( byte_at( row, offset ) );
                        return Rgb{ .red = gray, .green = gray, .blue = gray };
                    }
            }

            return {};
        }

        [[nodiscard]]
        std::uint8_t
        dimmed_grayscale( Rgb rgb ) noexcept
        {
            const auto average = ( rgb.red + rgb.green + rgb.blue ) / kRgbChannelCount;
            return static_cast<std::uint8_t>( average / kDimDivisor );
        }

        void
        set_output_pixel( std::vector<std::byte>& pixels,
                          std::size_t             offset,
                          std::uint8_t            red,
                          std::uint8_t            green,
                          std::uint8_t            blue,
                          std::uint8_t            alpha )
        {
            pixels.at( offset + kRedOffset )   = byte_from( red );
            pixels.at( offset + kGreenOffset ) = byte_from( green );
            pixels.at( offset + kBlueOffset )  = byte_from( blue );
            pixels.at( offset + kAlphaOffset ) = byte_from( alpha );
        }

    }

    grab::Result<DiffResult>
    compare( const grab::Image&    a,
             const grab::Image&    b,
             const CompareOptions& opts )
    {
        auto layout = validate_pair( a, b );
        if( !layout.has_value() )
        {
            return std::unexpected( layout.error() );
        }

        if( layout->total_pixels == 0U )
        {
            return DiffResult{};
        }

        std::uint64_t diff_pixels = 0U;
        std::uint32_t min_x       = a.width;
        std::uint32_t min_y       = a.height;
        std::uint32_t max_x       = 0U;
        std::uint32_t max_y       = 0U;

        for( std::uint32_t y = 0U; y < a.height; ++y )
        {
            const auto a_row = row_pixels( a, y, layout->row_bytes );
            const auto b_row = row_pixels( b, y, layout->row_bytes );
            for( std::uint32_t x = 0U; x < a.width; ++x )
            {
                const auto offset =
                    static_cast<std::size_t>( x ) * layout->bytes_per_pixel;
                if( pixel_differs( a_row,
                                   b_row,
                                   offset,
                                   layout->bytes_per_pixel,
                                   opts.per_channel_tolerance ) )
                {
                    ++diff_pixels;
                    min_x = std::min( min_x, x );
                    min_y = std::min( min_y, y );
                    max_x = std::max( max_x, x );
                    max_y = std::max( max_y, y );
                }
            }
        }

        std::optional<Rect> bounding_box;
        if( diff_pixels != 0U )
        {
            auto rect = rect_from_bounds( min_x, min_y, max_x, max_y );
            if( !rect.has_value() )
            {
                return std::unexpected( rect.error() );
            }
            bounding_box = *rect;
        }

        const auto matching_pixels = layout->total_pixels - diff_pixels;
        return DiffResult{
            .match_ratio  = static_cast<double>( matching_pixels ) /
                            static_cast<double>( layout->total_pixels ),
            .diff_pixels  = diff_pixels,
            .bounding_box = bounding_box,
        };
    }

    grab::Result<grab::Image>
    diff_image( const grab::Image& a,
                const grab::Image& b )
    {
        auto layout = validate_pair( a, b );
        if( !layout.has_value() )
        {
            return std::unexpected( layout.error() );
        }

        auto output_bytes = output_pixel_bytes( a.width, a.height );
        if( !output_bytes.has_value() )
        {
            return std::unexpected( output_bytes.error() );
        }

        const auto             stride = output_stride( a.width );
        std::vector<std::byte> pixels( *output_bytes );

        for( std::uint32_t y = 0U; y < a.height; ++y )
        {
            const auto a_row = row_pixels( a, y, layout->row_bytes );
            const auto b_row = row_pixels( b, y, layout->row_bytes );
            for( std::uint32_t x = 0U; x < a.width; ++x )
            {
                const auto input_offset =
                    static_cast<std::size_t>( x ) * layout->bytes_per_pixel;
                const auto output_offset =
                    ( static_cast<std::size_t>( y ) *
                      static_cast<std::size_t>( stride ) ) +
                    ( static_cast<std::size_t>( x ) * kRgbaBytes );
                if( pixel_differs( a_row,
                                   b_row,
                                   input_offset,
                                   layout->bytes_per_pixel,
                                   0U ) )
                {
                    set_output_pixel( pixels,
                                      output_offset,
                                      kHighlightRed,
                                      kHighlightGreen,
                                      kHighlightBlue,
                                      kOpaqueAlpha );
                }
                else
                {
                    const auto gray =
                        dimmed_grayscale( source_rgb( a_row, input_offset, a.format ) );
                    set_output_pixel( pixels,
                                      output_offset,
                                      gray,
                                      gray,
                                      gray,
                                      kOpaqueAlpha );
                }
            }
        }

        return grab::Image{
            .width  = a.width,
            .height = a.height,
            .stride = stride,
            .format = grab::PixelFormat::rgba,
            .pixels = std::move( pixels ),
        };
    }

}    // namespace grab::image
