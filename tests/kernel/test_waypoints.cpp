// PLACEHOLDER — registered in tests/CMakeLists.txt by Phase 0 so the drag-pacing
// unit can replace this file without touching a shared build file.

#include "grab/drag.hpp"
#include "grab/geometry/point.hpp"
#include "kernel/input/waypoints.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstddef>
// clang-format on

namespace
{

    constexpr std::size_t expectedWaypointCount =
        static_cast<std::size_t>( grab::input::DragOptions::defaultInterpolationSteps );

    TEST( Placeholder,
          WaypointsCompiles )
    {
        const grab::input::DragOptions options{};
        const auto points = grab::kernel::input::waypoints( grab::geometry::Point{},
                                                            grab::geometry::Point{},
                                                            options );
        EXPECT_EQ( points.size(), expectedWaypointCount );
    }

}    // namespace
