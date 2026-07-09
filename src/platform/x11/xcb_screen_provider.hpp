#pragma once

#include "grab/image.hpp"
#include "grab/result.hpp"
#include "platform/x11/xcb_connection.hpp"

#include <cstdint>

namespace grab::platform::x11
{

    [[nodiscard]]
    grab::Result<grab::Image>
    capture_region( const XcbConnection& conn,
                    std::int16_t         x,
                    std::int16_t         y,
                    std::uint16_t        width,
                    std::uint16_t        height,
                    bool                 draw_cursor );

}    // namespace grab::platform::x11
