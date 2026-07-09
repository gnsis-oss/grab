#pragma once

#include "grab/geometry/rectangle.hpp"
#include "grab/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

struct xcb_connection_t;

namespace grab::platform::x11
{

    struct CursorImage
    {
            std::uint32_t                  width     = 0U;
            std::uint32_t                  height    = 0U;
            std::int32_t                   hotspot_x = 0;
            std::int32_t                   hotspot_y = 0;
            std::int32_t                   xhot      = 0;
            std::int32_t                   yhot      = 0;
            std::span<const std::uint32_t> pixels;
    };

    void
    composite_cursor( std::span<std::uint8_t>          frame_bgr0,
                      std::size_t                      stride,
                      const grab::geometry::Rectangle& region,
                      const CursorImage&               cursor );

    [[nodiscard]]
    grab::Result<void>
    draw_xfixes_cursor( xcb_connection_t*                conn,
                        std::span<std::uint8_t>          frame_bgr0,
                        std::size_t                      stride,
                        const grab::geometry::Rectangle& region );

}    // namespace grab::platform::x11
