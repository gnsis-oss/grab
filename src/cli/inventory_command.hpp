#pragma once

#include <span>

namespace grab::cli
{

    int
    run_inventory_command( std::span<char* const> args );

}    // namespace grab::cli
