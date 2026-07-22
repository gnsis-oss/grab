#pragma once

#include "grab/geometry/rectangle.hpp"
#include "grab/result.hpp"
#include "grab/screen.hpp"
#include "grab/window_info.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace grab::cli
{

    // Parsed form of
    // `grab windows [--json] [--class WMCLASS] [--type TYPE] [--display D]`.
    struct WindowsOptions
    {
            bool        as_json = false;
            // Empty means "no filter": list every window.
            std::string wm_class;
            // Empty means "no filter"; otherwise an exact EWMH type name as
            // reported in WindowSummary::type, matched case-insensitively.
            std::string type;
            std::string display;
    };

    // A window selected either by WM_CLASS or by the exact id `grab windows`
    // reports. The id is the disambiguation escape hatch for applications that
    // own several windows of one class; exactly one of the two must be set.
    struct WindowSelector
    {
            std::string                  wm_class;
            std::optional<std::uint32_t> window_id;
    };

    // Parsed form of
    // `grab focus (--window WMCLASS | --window-id ID) [--display D]`.
    struct FocusOptions
    {
            WindowSelector selector;
            std::string    display;
    };

    // Parsed form of `grab place (--window WMCLASS | --window-id ID)
    // --geometry WxH+X+Y [--display D] [--timeout MS]`.
    struct PlaceOptions
    {
            WindowSelector            selector;
            grab::geometry::Rectangle geometry;
            std::string               display;
            std::chrono::milliseconds timeout = grab::Screen::defaultPlacementTimeout;
    };

    // Parses the `WxH+X+Y` spelling X11 tools use. Offsets may be negative;
    // dimensions may not be zero.
    [[nodiscard]]
    grab::Result<grab::geometry::Rectangle>
    parse_placement_geometry( std::string_view input );

    // Inverse of parse_placement_geometry, so a placement can be echoed back in
    // the spelling that requested it.
    [[nodiscard]]
    std::string
    format_geometry( const grab::geometry::Rectangle& bounds );

    [[nodiscard]]
    grab::Result<PlaceOptions>
    parse_place_options( std::span<char* const> args );

    // Resolves a selector to one window id against a window list: an id selector
    // must name a window that is present, a class selector takes the first match.
    [[nodiscard]]
    grab::Result<std::uint32_t>
    select_window_id( const WindowSelector&                   selector,
                      const std::vector<grab::WindowSummary>& windows );

    [[nodiscard]]
    grab::Result<WindowsOptions>
    parse_windows_options( std::span<char* const> args );

    [[nodiscard]]
    grab::Result<FocusOptions>
    parse_focus_options( std::span<char* const> args );

    // Filters by the same case-insensitive WM_CLASS substring rule the capture
    // path uses. An empty `wm_class` returns the input unchanged.
    [[nodiscard]]
    std::vector<grab::WindowSummary>
    filter_windows_by_class( std::vector<grab::WindowSummary> windows,
                             const std::string&               wm_class );

    // Filters on the exact EWMH type name, case-insensitively. Exact rather than
    // substring because the types are a closed vocabulary and "menu" must not
    // also select "popup_menu". An empty `type` returns the input unchanged.
    [[nodiscard]]
    std::vector<grab::WindowSummary>
    filter_windows_by_type( std::vector<grab::WindowSummary> windows,
                            const std::string&               type );

    // A JSON array of window objects, without a trailing newline.
    [[nodiscard]]
    std::string
    format_windows_json( const std::vector<grab::WindowSummary>& windows );

    // One `<id> <wm_class> pid=<pid> <x>,<y> <w>x<h> "<title>"` line per window,
    // newline-terminated. Empty when there are no windows.
    [[nodiscard]]
    std::string
    format_windows_text( const std::vector<grab::WindowSummary>& windows );

    int
    run_windows_command( std::span<char* const> args );

    int
    run_focus_command( std::span<char* const> args );

    int
    run_place_command( std::span<char* const> args );

}    // namespace grab::cli
