#pragma once

#include "grab/result.hpp"

#include <span>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <utility>
#include <vector>

namespace grab::inventory
{

    [[nodiscard]]
    grab::Result<pid_t>
    launch_app( std::string_view                           apprun,
                std::span<const std::string>               args,
                const std::vector<std::pair<std::string,
                                            std::string>>& environment );

    void
    terminate_app( pid_t pid );

}    // namespace grab::inventory
