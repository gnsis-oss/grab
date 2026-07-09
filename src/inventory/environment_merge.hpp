#pragma once

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace grab::inventory
{

    // Owned child environment: base entries minus QT_IM_MODULE and minus any key
    // an override replaces, then one KEY=VALUE per override (input order).
    [[nodiscard]]
    std::vector<std::string>
    merge_environment( std::span<const std::string_view>       base,
                       std::span<const std::pair<std::string,
                                                 std::string>> overrides );

}    // namespace grab::inventory
