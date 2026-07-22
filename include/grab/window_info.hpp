#pragma once

#include "grab/geometry/rectangle.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace grab
{

    // One mapped top-level window, as advertised by the desktop's window
    // manager. Purely descriptive: nothing here keeps the window alive, and a
    // summary goes stale the moment the window is moved, resized or destroyed.
    struct WindowSummary
    {
            // Native window handle (the X11 XID on X11 backends). Valid only for
            // the lifetime of the window; do not persist it across runs.
            std::uint32_t                id = 0U;

            // WM_CLASS class field verbatim, in the window's own casing. Callers
            // match against it case-insensitively by substring — see
            // Screen::activate_window_by_class.
            std::string                  wm_class;

            // _NET_WM_NAME (falling back to WM_NAME), UTF-8. Empty when unset.
            std::string                  title;

            // EWMH window type, lowercased and without the `_NET_WM_WINDOW_TYPE_`
            // prefix: "normal", "splash", "dialog", "utility", "dock", ... It is
            // the *first* entry of the window's declared type list that the
            // backend recognises, never a containment test: a splash screen
            // typically also advertises NORMAL as a fallback for window managers
            // that do not implement SPLASH, so only the client's own ordering
            // tells it apart from the real main window — which matters because
            // the two often share WM_CLASS, pid and title. A window declaring no
            // type at all reports "normal", as EWMH prescribes.
            std::string                  type;

            // Owning process, absent when the window advertises no _NET_WM_PID.
            std::optional<std::uint32_t> pid;

            // Outer frame in screen coordinates, so the origin is directly usable
            // for positional input without further translation.
            grab::geometry::Rectangle    bounds;
    };

}    // namespace grab
