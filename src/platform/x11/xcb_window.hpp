#pragma once

#include "grab/result.hpp"
#include "grab/window.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace grab::platform::x11
{

    class XcbConnection;

    struct WmClass
    {
            std::string instance;
            std::string window_class;
    };

    [[nodiscard]]
    WmClass
    parse_wm_class( std::span<const std::uint8_t> raw );

    [[nodiscard]]
    bool
    class_matches( const WmClass&   wc,
                   std::string_view app );

    [[nodiscard]]
    grab::Result<WindowRef>
    find_window( const XcbConnection& conn,
                 const WindowMatch&   match );

    [[nodiscard]]
    grab::Result<WindowRect>
    window_geometry( const XcbConnection& conn,
                     const WindowRef&     window );

}    // namespace grab::platform::x11
