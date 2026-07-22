#pragma once

#include <span>
#include <string>
#include <utility>
#include <vector>

namespace grab::config
{

    [[nodiscard]]
    std::vector<std::string>
    overlay_environment( std::span<const std::pair<std::string,
                                                   std::string>> overrides );

}    // namespace grab::config
