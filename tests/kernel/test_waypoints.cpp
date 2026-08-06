// The interpolation walk extracted out of the X11 drag recipe. These cases pin
// the contract the recipe relied on: the returned points EXCLUDE `from`,
// INCLUDE `to`, and number exactly options.interpolation_steps — for both the
// linear and the cubic path.

#include "grab/drag.hpp"
#include "grab/geometry/point.hpp"
#include "kernel/input/waypoints.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstddef>
#include <cstdint>
// clang-format on

namespace
{

    constexpr std::int32_t walkSteps             = 4;
    constexpr std::int32_t originX               = 100;
    constexpr std::int32_t originY               = 200;
    constexpr std::int32_t targetX               = 200;
    constexpr std::int32_t targetY               = 200;
    constexpr std::int32_t firstStepX            = 125;

    constexpr std::size_t expectedWalkPointCount = static_cast<std::size_t>( walkSteps );
    constexpr std::size_t expectedDefaultPointCount =
        static_cast<std::size_t>( grab::input::DragOptions::defaultInterpolationSteps );

    constexpr grab::geometry::Point origin{
        .x = originX,
        .y = originY,
    };
    constexpr grab::geometry::Point target{
        .x = targetX,
        .y = targetY,
    };

    [[nodiscard]]
    constexpr grab::input::DragOptions
    walk_options( grab::input::DragOptions::Path path ) noexcept
    {
        return grab::input::DragOptions{
            .interpolation_steps = walkSteps,
            .step_dwell          = grab::input::DragOptions::defaultStepDwell,
            .path                = path,
        };
    }

    TEST( Waypoints,
          LinearWalkStepsOffTheOriginAndLandsOnTheTarget )
    {
        const auto points = grab::kernel::input::waypoints(
            origin,
            target,
            walk_options( grab::input::DragOptions::Path::Linear )
        );

        ASSERT_EQ( points.size(), expectedWalkPointCount );
        EXPECT_EQ( points.front().x, firstStepX );
        EXPECT_EQ( points.front().y, originY );
        EXPECT_EQ( points.back(), target );
        EXPECT_NE( points.front(), origin );
    }

    TEST( Waypoints,
          CubicWalkStepsOffTheOriginAndLandsOnTheTarget )
    {
        const auto points = grab::kernel::input::waypoints(
            origin,
            target,
            walk_options( grab::input::DragOptions::Path::Cubic )
        );

        ASSERT_EQ( points.size(), expectedWalkPointCount );
        EXPECT_EQ( points.back(), target );
        EXPECT_NE( points.front(), origin );
    }

    TEST( Waypoints,
          LinearWalkAdvancesMonotonicallyFromTheOrigin )
    {
        const auto points = grab::kernel::input::waypoints(
            origin,
            target,
            walk_options( grab::input::DragOptions::Path::Linear )
        );

        ASSERT_EQ( points.size(), expectedWalkPointCount );
        std::int32_t previous_x = origin.x;
        for( const grab::geometry::Point point : points )
        {
            EXPECT_GT( point.x, previous_x );
            EXPECT_EQ( point.y, originY );
            previous_x = point.x;
        }
    }

    TEST( Waypoints,
          CubicWalkAdvancesMonotonicallyFromTheOrigin )
    {
        const auto points = grab::kernel::input::waypoints(
            origin,
            target,
            walk_options( grab::input::DragOptions::Path::Cubic )
        );

        ASSERT_EQ( points.size(), expectedWalkPointCount );
        std::int32_t previous_x = origin.x;
        for( const grab::geometry::Point point : points )
        {
            EXPECT_GT( point.x, previous_x );
            EXPECT_EQ( point.y, originY );
            previous_x = point.x;
        }
    }

    TEST( Waypoints,
          DefaultOptionsYieldOnePointPerInterpolationStep )
    {
        const grab::input::DragOptions options{};
        const auto points = grab::kernel::input::waypoints( grab::geometry::Point{},
                                                            grab::geometry::Point{},
                                                            options );

        EXPECT_EQ( points.size(), expectedDefaultPointCount );
    }

}    // namespace
