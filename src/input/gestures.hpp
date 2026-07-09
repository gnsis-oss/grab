#ifndef GRAB_INPUT_GESTURES_HPP
#define GRAB_INPUT_GESTURES_HPP

#include "grab/geometry/point.hpp"
#include "grab/result.hpp"
#include "input/seat.hpp"

#include <chrono>
#include <cstdint>

namespace grab::input
{

    // Unified geometry point (int32 pixel coordinates). Shared with the
    // input_sink/gesture stack so a single grab::input::Point type exists.
    using Point = geometry::Point;

    struct QtDragParams
    {
            std::int32_t              interpolation_steps = 16;
            std::chrono::milliseconds step_dwell{ 8 };
            std::chrono::milliseconds drag_start_dwell{ 50 };
    };

    [[nodiscard]]
    grab::Result<void>
    qt_drag( Seat&               seat,
             Point               from,
             Point               to,
             const QtDragParams& params = {} );

    [[nodiscard]]
    grab::Result<void>
    menu_click( Seat& seat,
                Point item );

}    // namespace grab::input

#endif    // GRAB_INPUT_GESTURES_HPP
