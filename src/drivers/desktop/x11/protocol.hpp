#pragma once

#include <string_view>

namespace grab::platform::x11::atom_name
{

    inline constexpr std::string_view netActiveWindow = "_NET_ACTIVE_WINDOW";
    inline constexpr std::string_view netClientList   = "_NET_CLIENT_LIST";
    inline constexpr std::string_view netClientListStacking =
        "_NET_CLIENT_LIST_STACKING";
    inline constexpr std::string_view netMoveresizeWindow  = "_NET_MOVERESIZE_WINDOW";
    inline constexpr std::string_view netWmState           = "_NET_WM_STATE";
    inline constexpr std::string_view netWmStateFullscreen = "_NET_WM_STATE_FULLSCREEN";
    inline constexpr std::string_view netWmStateMaximizedHorz =
        "_NET_WM_STATE_MAXIMIZED_HORZ";
    inline constexpr std::string_view netWmStateMaximizedVert =
        "_NET_WM_STATE_MAXIMIZED_VERT";
    inline constexpr std::string_view netWmWindowType = "_NET_WM_WINDOW_TYPE";
    inline constexpr std::string_view netWmName       = "_NET_WM_NAME";
    inline constexpr std::string_view netWmPid        = "_NET_WM_PID";
    inline constexpr std::string_view utf8String      = "UTF8_STRING";
    inline constexpr std::string_view wmClass         = "WM_CLASS";

}    // namespace grab::platform::x11::atom_name
