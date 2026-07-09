#pragma once

#include "grab/image.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace grab
{

    template<PixelFormat Format>
    struct PixelTraits;

    template<std::size_t BytesPerPixel,
             std::size_t RedOffset,
             std::size_t GreenOffset,
             std::size_t BlueOffset>
    struct RgbBytePixelTraits
    {
            static constexpr std::size_t bytes_per_pixel = BytesPerPixel;

            [[nodiscard]]
            static std::uint8_t
            red( std::span<const std::uint8_t> pixel ) noexcept
            {
                return channel( pixel, RedOffset );
            }

            [[nodiscard]]
            static std::uint8_t
            green( std::span<const std::uint8_t> pixel ) noexcept
            {
                return channel( pixel, GreenOffset );
            }

            [[nodiscard]]
            static std::uint8_t
            blue( std::span<const std::uint8_t> pixel ) noexcept
            {
                return channel( pixel, BlueOffset );
            }

            static void
            write_rgb( std::span<std::uint8_t> pixel,
                       std::uint8_t            red_value,
                       std::uint8_t            green_value,
                       std::uint8_t            blue_value ) noexcept
            {
                set_channel( pixel, RedOffset, red_value );
                set_channel( pixel, GreenOffset, green_value );
                set_channel( pixel, BlueOffset, blue_value );
            }

        private:

            static constexpr std::size_t single_channel = 1U;

            [[nodiscard]]
            static std::uint8_t
            channel( std::span<const std::uint8_t> pixel,
                     std::size_t                   offset ) noexcept
            {
                return pixel.subspan( offset, single_channel ).front();
            }

            static void
            set_channel( std::span<std::uint8_t> pixel,
                         std::size_t             offset,
                         std::uint8_t            value ) noexcept
            {
                pixel.subspan( offset, single_channel ).front() = value;
            }
    };

    template<>
    struct PixelTraits<PixelFormat::rgb24> : RgbBytePixelTraits<3U, 0U, 1U, 2U>
    {
    };

    template<>
    struct PixelTraits<PixelFormat::bgr0> : RgbBytePixelTraits<4U, 2U, 1U, 0U>
    {
    };

}    // namespace grab
