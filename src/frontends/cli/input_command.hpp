#pragma once

#include "grab/geometry/point.hpp"
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

    // THE PARITY FORCING FUNCTION. `grab click --at`, `grab type` and
    // `grab drag` do their work by building a one-step Sequence and playing
    // it, rather than by calling grab::Input directly. Without a real consumer
    // the command layer drifts behind grab::Input -- which is exactly what the
    // descriptor table had already done, covering 5 of Input's 12 operations.
    //
    // `grab drag-curve` CANNOT be routed: there is no DragCurveCommand among
    // the fifteen alternatives of grab::sequence::Command, so it keeps its own
    // path through Session::perform above. This is parity for three verbs, not
    // for all four.
    [[nodiscard]]
    grab::Result<void>
    play_click_at( const char*           display,
                   grab::geometry::Point at,
                   std::uint8_t          button );

    [[nodiscard]]
    grab::Result<void>
    play_type_text( const char*      display,
                    std::string_view layout,
                    std::string_view text );

    [[nodiscard]]
    grab::Result<void>
    play_drag( const char*           display,
               grab::geometry::Point from,
               grab::geometry::Point to );

}    // namespace grab::cli
