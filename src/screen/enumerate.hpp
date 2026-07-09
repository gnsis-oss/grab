#pragma once

#include "grab/geometry/rectangle.hpp"
#include "grab/result.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace grab::screen
{

    struct WindowInfo
    {
            std::uint32_t             id = 0U;
            std::string               wm_class;
            std::string               title;
            grab::geometry::Rectangle bounds;
    };

    [[nodiscard]]
    grab::Result<std::vector<WindowInfo>>
    list_windows( const char* display = nullptr );

    struct OutputInfo
    {
            std::string               name;
            grab::geometry::Rectangle bounds;
    };

    [[nodiscard]]
    grab::Result<std::vector<OutputInfo>>
    list_outputs( const char* display = nullptr );

}    // namespace grab::screen
