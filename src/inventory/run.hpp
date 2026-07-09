#pragma once

#include "inventory/manifest.hpp"

#include <string_view>
#include <vector>

namespace grab::inventory
{

    [[nodiscard]]
    std::vector<Entry>
    capture_live( std::string_view root,
                  std::string_view apprun,
                  std::string_view out_dir );

}    // namespace grab::inventory
