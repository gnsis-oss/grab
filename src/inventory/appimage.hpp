#pragma once

#include "grab/result.hpp"

#include <string>
#include <string_view>

namespace grab::inventory
{

    [[nodiscard]]
    grab::Result<std::string>
    extract_appimage( std::string_view appimage_path,
                      std::string_view cache_dir );

    [[nodiscard]]
    grab::Result<std::string>
    find_appimage( std::string_view root,
                   std::string_view override_path );

}    // namespace grab::inventory
