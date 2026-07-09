#pragma once

#include "grab/result.hpp"
#include "inventory/action.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace grab::inventory
{

    [[nodiscard]]
    std::optional<std::string>
    resolve_sample( std::string_view root,
                    std::string_view key );

    [[nodiscard]]
    grab::Result<std::vector<Step>>
    resolve_step_samples( const std::vector<Step>& steps,
                          std::string_view         root );

}    // namespace grab::inventory
