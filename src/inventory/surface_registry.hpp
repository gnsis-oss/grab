#pragma once

#include "inventory/surface.hpp"

#include <optional>
#include <string_view>
#include <vector>

namespace grab::inventory
{

    [[nodiscard]]
    const std::vector<Surface>&
    all_surfaces();

    [[nodiscard]]
    std::optional<std::string_view>
    surface_sample_rel( std::string_view key ) noexcept;

}    // namespace grab::inventory
