#pragma once

#include "grab/geometry/size.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace grab
{

    enum class PixelFormat : std::uint8_t
    {
        Bgra,
        Rgba,
        Rgb,
        Bgr,
        Gray,
    };

    [[nodiscard]]
    constexpr std::uint32_t
    bytes_per_pixel( PixelFormat format ) noexcept
    {
        constexpr std::uint32_t fourChannelBytes  = 4U;
        constexpr std::uint32_t threeChannelBytes = 3U;
        constexpr std::uint32_t grayBytes         = 1U;

        switch( format )
        {
            case PixelFormat::Bgra :
            case PixelFormat::Rgba :
                return fourChannelBytes;
            case PixelFormat::Rgb :
            case PixelFormat::Bgr :
                return threeChannelBytes;
            case PixelFormat::Gray :
                return grayBytes;
        }

        return grayBytes;
    }

    struct Image
    {
            std::uint32_t          width  = 0U;
            std::uint32_t          height = 0U;
            std::uint32_t          stride = 0U;
            PixelFormat            format = PixelFormat::Rgba;
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

            [[nodiscard]]
            std::span<const std::byte>
            row( std::uint32_t y ) const
            {
                if( y >= height )
                {
                    return {};
                }

                constexpr auto maxSize     = std::numeric_limits<std::size_t>::max();
                const auto     stride_size = static_cast<std::size_t>( stride );
                const auto     row_index   = static_cast<std::size_t>( y );
                if( stride_size != 0U && row_index > maxSize / stride_size )
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
