#pragma once

#include "grab/result.hpp"

#include <cstdint>
#include <span>
#include <string_view>

namespace grab::cli
{

    // Maps a fraction in [0,1] along one window axis (origin + extent) to an
    // absolute int16 screen coordinate: fraction 0 -> origin, fraction 1 ->
    // origin + extent - 1. Rejects non-finite inputs, out-of-range fractions,
    // sub-pixel extents, and coordinates outside int16.
    [[nodiscard]]
    grab::Result<std::int16_t>
    window_fraction_to_coordinate( double           origin,
                                   double           size,
                                   double           fraction,
                                   std::string_view axis );

    int
    run_key_command( std::span<char* const> args );

    int
    run_drag_curve_command( std::span<char* const> args );

}    // namespace grab::cli
