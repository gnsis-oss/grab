#pragma once

#include "grab/geometry/rectangle.hpp"

#include <cstdint>
#include <string>

namespace grab
{

    struct WindowRef
    {
            std::uint32_t id    = 0U;
            bool          valid = false;
    };

    struct WindowMatch
    {
            std::string app;
    };

    using WindowRect = geometry::Rectangle;

}    // namespace grab
