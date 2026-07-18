#include "grab/event.hpp"
#include "grab/origin.hpp"
#include "grab/overlay.hpp"
#include "grab/space.hpp"
#include "grab/watch.hpp"
#include "kernel/presentation/overlay_scene.hpp"
#include "kernel/presentation/trail_animator.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <variant>
// clang-format on

namespace
{

    constexpr grab::CoordinateSpaceId   firstSpace{ 7U };
    constexpr grab::CoordinateSpaceId   secondSpace{ 8U };
    constexpr grab::SpacePoint          firstPoint{ 10.0, 20.0, firstSpace };
    constexpr grab::SpacePoint          secondPoint{ 20.0, 30.0, firstSpace };
    constexpr grab::SpacePoint          thirdPoint{ 30.0, 40.0, firstSpace };
    constexpr grab::SpacePoint          fourthPoint{ 40.0, 50.0, firstSpace };
    constexpr std::chrono::milliseconds sceneTime{ 100 };
    constexpr std::chrono::milliseconds clockStep{ 1 };
    constexpr std::size_t               noShapes           = 0U;
    constexpr std::size_t               oneShape           = 1U;
    constexpr std::size_t               twoShapes          = 2U;
    constexpr std::size_t               twoPathCommands    = 2U;
    constexpr std::size_t               firstShapeIndex    = 0U;
    constexpr std::size_t               secondShapeIndex   = 1U;
    constexpr std::size_t               firstCommandIndex  = 0U;
    constexpr std::size_t               secondCommandIndex = 1U;
    constexpr std::uint64_t             oneFailure         = 1U;
    constexpr double                    zeroDelta{};
    constexpr double notANumber = std::numeric_limits<double>::quiet_NaN();

    [[nodiscard]]
    grab::SubscriptionEvent
    motion( grab::EventOrigin       origin,
            const grab::SpacePoint& position )
    {
        return grab::Event{
            .kind     = grab::EventKind::MouseMove,
            .category = grab::EventCategory::Input,
            .payload =
                grab::MouseMove{
                                .axis     = "x",
                                .delta    = zeroDelta,
                                .position = position,
                                },
            .origin = origin,
        };
    }

    [[nodiscard]]
    grab::SubscriptionEvent
    positionless_motion( grab::EventOrigin origin )
    {
        return grab::Event{
            .kind     = grab::EventKind::MouseMove,
            .category = grab::EventCategory::Input,
            .payload =
                grab::MouseMove{
                                .axis     = "x",
                                .delta    = zeroDelta,
                                .position = {},
                                },
            .origin = origin,
        };
    }

    [[nodiscard]]
    grab::SubscriptionEvent
    key_event()
    {
        return grab::Event{
            .kind     = grab::EventKind::KeyDown,
            .category = grab::EventCategory::Input,
            .payload  = grab::InputKey{},
            .origin   = grab::EventOrigin::Physical,
        };
    }

    void
    expect_color( const grab::overlay::Color& actual,
                  const grab::overlay::Color& expected )
    {
        EXPECT_EQ( actual.r, expected.r );
        EXPECT_EQ( actual.g, expected.g );
        EXPECT_EQ( actual.b, expected.b );
        EXPECT_EQ( actual.a, expected.a );
    }

    void
    expect_point( const grab::SpacePoint& actual,
                  const grab::SpacePoint& expected )
    {
        EXPECT_DOUBLE_EQ( actual.x, expected.x );
        EXPECT_DOUBLE_EQ( actual.y, expected.y );
        EXPECT_EQ( actual.space, expected.space );
    }

    void
    expect_segment( const grab::overlay::ShapeRecord& record,
                    const grab::SpacePoint&           from,
                    const grab::SpacePoint&           to,
                    const grab::overlay::Color&       color )
    {
        const auto& shape = record.shape;
        EXPECT_EQ( shape.band, grab::overlay::Band::Trail );
        EXPECT_FALSE( shape.fill.has_value() );
        ASSERT_TRUE( shape.stroke.has_value() );
        const auto stroke = shape.stroke.value_or( grab::overlay::StrokeStyle{} );
        expect_color( stroke.color, color );
        EXPECT_FLOAT_EQ( stroke.width_px,
                         grab::kernel::presentation::defaultTrailWidthPx );

        const auto* fade = std::get_if<grab::overlay::Fade>( &shape.lifetime );
        ASSERT_NE( fade, nullptr );
        EXPECT_EQ( fade->duration, grab::kernel::presentation::defaultTrailFade );

        const auto* path = std::get_if<grab::overlay::Path>( &shape.geometry );
        ASSERT_NE( path, nullptr );
        ASSERT_EQ( path->commands.size(), twoPathCommands );
        EXPECT_FALSE( path->closed );
        const auto* move = std::get_if<grab::overlay::MoveTo>(
            &path->commands.at( firstCommandIndex )
        );
        const auto* line = std::get_if<grab::overlay::LineTo>(
            &path->commands.at( secondCommandIndex )
        );
        ASSERT_NE( move, nullptr );
        ASSERT_NE( line, nullptr );
        expect_point( move->point, from );
        expect_point( line->point, to );
    }

}    // namespace

TEST( TrailAnimator,
      ThreePhysicalMotionsAppendTwoOrderedDefaultSegments )
{
    auto                                      now = sceneTime;
    grab::kernel::presentation::OverlayScene  scene{ [&now]
                                                     {
                                                        const auto current  = now;
                                                        now                += clockStep;
                                                        return current;
                                                     } };
    grab::kernel::presentation::TrailAnimator animator{
        scene,
        grab::kernel::presentation::TrailStyle{},
    };

    animator.consume( motion( grab::EventOrigin::Physical, firstPoint ) );
    animator.consume( motion( grab::EventOrigin::Physical, secondPoint ) );
    animator.consume( motion( grab::EventOrigin::Physical, thirdPoint ) );

    const auto snapshot = scene.snapshot();
    ASSERT_EQ( snapshot.shapes.size(), twoShapes );
    expect_segment( snapshot.shapes.at( firstShapeIndex ),
                    firstPoint,
                    secondPoint,
                    grab::kernel::presentation::defaultPhysicalTrailColor );
    expect_segment( snapshot.shapes.at( secondShapeIndex ),
                    secondPoint,
                    thirdPoint,
                    grab::kernel::presentation::defaultPhysicalTrailColor );
    EXPECT_LT( snapshot.shapes.at( firstShapeIndex ).started_at,
               snapshot.shapes.at( secondShapeIndex ).started_at );
}

TEST( TrailAnimator,
      OriginTransitionDoesNotBridgeAndUsesInjectedColor )
{
    grab::kernel::presentation::OverlayScene  scene{ []
                                                     {
                                                        return sceneTime;
                                                     } };
    grab::kernel::presentation::TrailAnimator animator{
        scene,
        grab::kernel::presentation::TrailStyle{},
    };

    animator.consume( motion( grab::EventOrigin::Physical, firstPoint ) );
    animator.consume( motion( grab::EventOrigin::Physical, secondPoint ) );
    animator.consume( motion( grab::EventOrigin::InjectedSelf, thirdPoint ) );
    animator.consume( motion( grab::EventOrigin::InjectedSelf, fourthPoint ) );

    const auto snapshot = scene.snapshot();
    ASSERT_EQ( snapshot.shapes.size(), twoShapes );
    expect_segment( snapshot.shapes.at( firstShapeIndex ),
                    firstPoint,
                    secondPoint,
                    grab::kernel::presentation::defaultPhysicalTrailColor );
    expect_segment( snapshot.shapes.at( secondShapeIndex ),
                    thirdPoint,
                    fourthPoint,
                    grab::kernel::presentation::defaultInjectedTrailColor );
}

TEST( TrailAnimator,
      QueueGapBreaksPath )
{
    grab::kernel::presentation::OverlayScene  scene{ []
                                                     {
                                                        return sceneTime;
                                                     } };
    grab::kernel::presentation::TrailAnimator animator{
        scene,
        grab::kernel::presentation::TrailStyle{},
    };

    animator.consume( motion( grab::EventOrigin::Physical, firstPoint ) );
    animator.consume( motion( grab::EventOrigin::Physical, secondPoint ) );
    animator.consume( grab::QueueGapMarker{} );
    animator.consume( motion( grab::EventOrigin::Physical, thirdPoint ) );
    animator.consume( motion( grab::EventOrigin::Physical, fourthPoint ) );

    const auto snapshot = scene.snapshot();
    ASSERT_EQ( snapshot.shapes.size(), twoShapes );
    expect_segment( snapshot.shapes.at( secondShapeIndex ),
                    thirdPoint,
                    fourthPoint,
                    grab::kernel::presentation::defaultPhysicalTrailColor );
}

TEST( TrailAnimator,
      SpaceChangeBreaksPath )
{
    grab::kernel::presentation::OverlayScene  scene{ []
                                                     {
                                                        return sceneTime;
                                                     } };
    grab::kernel::presentation::TrailAnimator animator{
        scene,
        grab::kernel::presentation::TrailStyle{},
    };
    const grab::SpacePoint thirdInSecondSpace{
        thirdPoint.x,
        thirdPoint.y,
        secondSpace,
    };
    const grab::SpacePoint fourthInSecondSpace{
        fourthPoint.x,
        fourthPoint.y,
        secondSpace,
    };

    animator.consume( motion( grab::EventOrigin::Physical, firstPoint ) );
    animator.consume( motion( grab::EventOrigin::Physical, secondPoint ) );
    animator.consume( motion( grab::EventOrigin::Physical, thirdInSecondSpace ) );
    animator.consume( motion( grab::EventOrigin::Physical, fourthInSecondSpace ) );

    const auto snapshot = scene.snapshot();
    ASSERT_EQ( snapshot.shapes.size(), twoShapes );
    expect_segment( snapshot.shapes.at( secondShapeIndex ),
                    thirdInSecondSpace,
                    fourthInSecondSpace,
                    grab::kernel::presentation::defaultPhysicalTrailColor );
}

TEST( TrailAnimator,
      PositionlessMotionBreaksAndAppendsNothing )
{
    grab::kernel::presentation::OverlayScene  scene{ []
                                                     {
                                                        return sceneTime;
                                                     } };
    grab::kernel::presentation::TrailAnimator animator{
        scene,
        grab::kernel::presentation::TrailStyle{},
    };

    animator.consume( motion( grab::EventOrigin::Physical, firstPoint ) );
    animator.consume( positionless_motion( grab::EventOrigin::Physical ) );
    animator.consume( motion( grab::EventOrigin::Physical, secondPoint ) );
    animator.consume( motion( grab::EventOrigin::Physical, thirdPoint ) );

    const auto snapshot = scene.snapshot();
    ASSERT_EQ( snapshot.shapes.size(), oneShape );
    expect_segment( snapshot.shapes.front(),
                    secondPoint,
                    thirdPoint,
                    grab::kernel::presentation::defaultPhysicalTrailColor );
}

TEST( TrailAnimator,
      OtherOriginsBreakAndNonMotionEventsAreIgnored )
{
    grab::kernel::presentation::OverlayScene  scene{ []
                                                     {
                                                        return sceneTime;
                                                     } };
    grab::kernel::presentation::TrailAnimator animator{
        scene,
        grab::kernel::presentation::TrailStyle{},
    };

    animator.consume( motion( grab::EventOrigin::Physical, firstPoint ) );
    animator.consume( key_event() );
    animator.consume( motion( grab::EventOrigin::Physical, secondPoint ) );
    animator.consume( motion( grab::EventOrigin::InjectedOther, thirdPoint ) );
    animator.consume( motion( grab::EventOrigin::Physical, thirdPoint ) );
    animator.consume( motion( grab::EventOrigin::Physical, fourthPoint ) );

    const auto snapshot = scene.snapshot();
    ASSERT_EQ( snapshot.shapes.size(), twoShapes );
    expect_segment( snapshot.shapes.at( secondShapeIndex ),
                    thirdPoint,
                    fourthPoint,
                    grab::kernel::presentation::defaultPhysicalTrailColor );
}

TEST( TrailAnimator,
      SceneAddFailuresAreSwallowedAndCounted )
{
    grab::kernel::presentation::OverlayScene  scene{ []
                                                     {
                                                        return sceneTime;
                                                     } };
    grab::kernel::presentation::TrailAnimator animator{
        scene,
        grab::kernel::presentation::TrailStyle{},
    };
    const grab::SpacePoint invalidPoint{ notANumber, secondPoint.y, firstSpace };

    animator.consume( motion( grab::EventOrigin::Physical, firstPoint ) );
    animator.consume( motion( grab::EventOrigin::Physical, invalidPoint ) );

    EXPECT_EQ( scene.snapshot().shapes.size(), noShapes );
    EXPECT_EQ( animator.scene_add_failure_count(), oneFailure );
}
