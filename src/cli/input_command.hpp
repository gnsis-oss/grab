#pragma once

#include <span>

namespace grab::cli
{

    int
    run_key_command( std::span<char* const> args );

    int
    run_drag_curve_command( std::span<char* const> args );

}    // namespace grab::cli
