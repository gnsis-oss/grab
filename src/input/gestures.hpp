#pragma once

#include "drivers/desktop/x11/x11_xtest_seat.hpp"
#include "grab/drag.hpp"
#include "grab/geometry/point.hpp"
#include "grab/result.hpp"

namespace grab::input
{

    // Unified geometry point (int32 pixel coordinates).
    using Point = geometry::Point;

    [[nodiscard]]
    grab::Result<void>
    linear_drag( Seat&              seat,
                 Point              from,
                 Point              to,
                 const DragOptions& options = {} );

    [[nodiscard]]
    grab::Result<void>
    curve_drag( Seat&              seat,
                Point              from,
                Point              to,
                const DragOptions& options = {} );

    [[nodiscard]]
    grab::Result<void>
    menu_click( Seat& seat,
                Point item );

}    // namespace grab::input
