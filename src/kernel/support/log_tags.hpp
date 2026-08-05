#pragma once

// Subsystem tags, so `--log-tags frame,present` can narrow a trace to the
// pipeline stage under investigation. Compile-time constants, not strings
// built at the call site.
//
// Keep these stable: they are the vocabulary a user types on the command
// line and the key an agent greps a captured log by.

#include <string_view>

namespace grab::log::tags
{

    // Scheduling
    inline constexpr std::string_view reactor = "reactor";

    // Input observation
    inline constexpr std::string_view events = "events";
    inline constexpr std::string_view input  = "input";

    // Overlay pipeline, in the order a frame moves through it
    inline constexpr std::string_view overlay = "overlay";
    inline constexpr std::string_view scene   = "scene";
    inline constexpr std::string_view raster  = "raster";
    inline constexpr std::string_view present = "present";
    inline constexpr std::string_view frame   = "frame";

    // Interaction
    inline constexpr std::string_view edit  = "edit";
    inline constexpr std::string_view trail = "trail";

    // Lifecycle
    inline constexpr std::string_view session = "session";

}    // namespace grab::log::tags
