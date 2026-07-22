#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/overlay.hpp"
#include "grab/result.hpp"

#include <chrono>
#include <optional>
#include <span>
#include <string_view>

namespace grab::cli
{

    struct OverlayTrailOptions
    {
            overlay::Color            physical_color{};
            overlay::Color            injected_color{};
            std::chrono::milliseconds fade{};
            float                     width_px{};
    };

    struct OverlayShapeRequest
    {
            overlay::Shape                           shape;
            std::optional<std::chrono::milliseconds> wait_for;
    };

    [[nodiscard]]
    Result<OverlayTrailOptions>
    parse_overlay_trail_options( std::span<const std::string_view> args );

    [[nodiscard]]
    Result<OverlayShapeRequest>
    parse_overlay_shape_options( std::string_view                  verb,
                                 std::span<const std::string_view> args );

    int
    run_overlay_command( std::span<char* const> args );

    int
    run_trail_command( std::span<char* const> args );

}    // namespace grab::cli
