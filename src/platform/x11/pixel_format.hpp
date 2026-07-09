#pragma once

#include "grab/image.hpp"
#include "grab/result.hpp"

#include <cstdint>

namespace grab::platform::x11
{

    struct XPixmapFormat
    {
            std::uint8_t depth          = 0U;
            std::uint8_t bits_per_pixel = 0U;
    };

    [[nodiscard]]
    grab::Result<grab::PixelFormat>
    pixel_format_for( std::uint8_t  depth,
                      std::uint8_t  bits_per_pixel,
                      std::uint32_t image_byte_order );

}    // namespace grab::platform::x11
