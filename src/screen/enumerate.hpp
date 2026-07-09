#pragma once

#include "grab/result.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace grab::screen
{

    struct WindowInfo
    {
            std::uint32_t id = 0U;
            std::string   wm_class;
            std::string   title;
            std::int16_t  x      = 0;
            std::int16_t  y      = 0;
            std::uint16_t width  = 0U;
            std::uint16_t height = 0U;
    };

    [[nodiscard]]
    grab::Result<std::vector<WindowInfo>>
    list_windows( const char* display = nullptr );

    struct OutputInfo
    {
            std::string   name;
            std::int16_t  x      = 0;
            std::int16_t  y      = 0;
            std::uint16_t width  = 0U;
            std::uint16_t height = 0U;
    };

    [[nodiscard]]
    grab::Result<std::vector<OutputInfo>>
    list_outputs( const char* display = nullptr );

}    // namespace grab::screen
