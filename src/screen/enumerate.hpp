#pragma once

#include "grab/geometry/rectangle.hpp"
#include "grab/result.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <xcb/xcb.h>

namespace grab::screen
{

    struct WindowInfo
    {
            std::uint32_t                id = 0U;
            std::string                  wm_class;
            std::string                  title;
            std::optional<std::uint32_t> pid;
            grab::geometry::Rectangle    bounds;
    };

    [[nodiscard]]
    grab::Result<std::vector<WindowInfo>>
    list_windows( const char* display = nullptr );

    [[nodiscard]]
    grab::Result<std::vector<WindowInfo>>
    list_windows( xcb_connection_t* connection,
                  xcb_window_t      root );

    struct OutputInfo
    {
            std::string               name;
            grab::geometry::Rectangle bounds;
    };

    [[nodiscard]]
    grab::Result<std::vector<OutputInfo>>
    list_outputs( const char* display = nullptr );

}    // namespace grab::screen
