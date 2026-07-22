#pragma once

#include "grab/result.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace grab::config
{

    struct PatternContext
    {
            std::chrono::system_clock::time_point now{};
            std::uint32_t                         seq{};
    };

    [[nodiscard]]
    grab::Result<std::string>
    render_filename( std::string_view      pattern,
                     const PatternContext& context );

    [[nodiscard]]
    bool
    matches_pattern( std::string_view pattern,
                     std::string_view name );

}    // namespace grab::config
