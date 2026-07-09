#ifndef GRAB_IMAGE_HPP
#define GRAB_IMAGE_HPP

#include "grab/geometry/size.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace grab
{

    // Unified pixel-format set: sg-port's channel-order family plus the two
    // X11-capture formats (rgb24 = packed RGB, bgr0 = 32-bit BGRX) that the
    // integrated platform/x11 + png_encoder path produces.
    enum class PixelFormat : std::uint8_t
    {
        bgra,
        rgba,
        rgb,
        bgr,
        gray,
        rgb24,
        bgr0,
    };

    [[nodiscard]]
    constexpr std::uint32_t
    bytes_per_pixel( PixelFormat format ) noexcept
    {
        constexpr std::uint32_t kFourChannelBytes  = 4U;
        constexpr std::uint32_t kThreeChannelBytes = 3U;
        constexpr std::uint32_t kGrayBytes         = 1U;

        switch( format )
        {
            case PixelFormat::bgra :
            case PixelFormat::rgba :
            case PixelFormat::bgr0 :
                return kFourChannelBytes;
            case PixelFormat::rgb :
            case PixelFormat::bgr :
            case PixelFormat::rgb24 :
                return kThreeChannelBytes;
            case PixelFormat::gray :
                return kGrayBytes;
        }

        return kGrayBytes;
    }

    // Non-owning view over pixel data, used by the integrated png_encoder and
    // platform/x11 capture path.
    struct ImageView
    {
            const std::uint8_t* data   = nullptr;
            std::uint32_t       width  = 0U;
            std::uint32_t       height = 0U;
            std::size_t         stride = 0U;
            PixelFormat         format = PixelFormat::rgb24;

            [[nodiscard]]
            constexpr geometry::Size
            size() const noexcept
            {
                return geometry::Size{ .width = width, .height = height };
            }

            [[nodiscard]]
            static constexpr ImageView
            from_size( const std::uint8_t* image_data,
                       geometry::Size      image_size,
                       std::size_t         image_stride,
                       PixelFormat         image_format = PixelFormat::rgb24 ) noexcept
            {
                return ImageView{
                    .data   = image_data,
                    .width  = image_size.width,
                    .height = image_size.height,
                    .stride = image_stride,
                    .format = image_format,
                };
            }
    };

    struct Image
    {
            std::uint32_t          width  = 0U;
            std::uint32_t          height = 0U;
            std::uint32_t          stride = 0U;
            PixelFormat            format = PixelFormat::rgba;
            std::vector<std::byte> pixels;

            [[nodiscard]]
            bool
            empty() const noexcept
            {
                return width == 0U || height == 0U;
            }

            [[nodiscard]]
            constexpr geometry::Size
            size() const noexcept
            {
                return geometry::Size{ .width = width, .height = height };
            }

            // View the pixel buffer as raw bytes (for the ImageView-based
            // encode path).
            [[nodiscard]]
            ImageView
            view() const noexcept
            {
                return ImageView{
                    .data   = reinterpret_cast<const std::uint8_t*>( pixels.data() ),
                    .width  = width,
                    .height = height,
                    .stride = stride,
                    .format = format,
                };
            }

            [[nodiscard]]
            std::span<const std::byte>
            row( std::uint32_t y ) const
            {
                if( y >= height )
                {
                    return {};
                }

                constexpr auto kMaxSize    = std::numeric_limits<std::size_t>::max();
                const auto     stride_size = static_cast<std::size_t>( stride );
                const auto     row_index   = static_cast<std::size_t>( y );
                if( stride_size != 0U && row_index > kMaxSize / stride_size )
                {
                    return {};
                }

                const auto offset = row_index * stride_size;
                if( offset > pixels.size() || pixels.size() - offset < stride_size )
                {
                    return {};
                }

                const std::span<const std::byte> pixel_span{ pixels };
                return pixel_span.subspan( offset, stride_size );
            }
    };

}    // namespace grab

#endif    // GRAB_IMAGE_HPP
