#include "grab/event.hpp"
#include "grab/space.hpp"
#include "kernel/input/gesture_classifier.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>
// clang-format on

namespace
{

    namespace gesture  = grab::kernel::input;

    using Milliseconds = std::chrono::milliseconds;

    constexpr Milliseconds  startTime{};
    constexpr Milliseconds  eventStep{ 100 };
    constexpr Milliseconds  boundaryStep{ 1 };
    constexpr Milliseconds  holdThreshold{ 500 };
    constexpr Milliseconds  doubleClickThreshold{ 400 };
    constexpr Milliseconds  pauseThreshold{ 700 };
    constexpr Milliseconds  insideDoubleClick{ 300 };
    constexpr Milliseconds  longAdvance{ 2'000 };
    constexpr Milliseconds  beforeHold       = holdThreshold - boundaryStep;
    constexpr Milliseconds  afterDoubleClick = doubleClickThreshold + boundaryStep;

    constexpr double        slopPx           = 5.0;
    constexpr double        zeroDelta{};

    constexpr std::uint32_t noButton         = 0U;
    constexpr std::uint32_t primaryButton    = 1U;
    constexpr std::uint32_t middleButton     = 2U;
    constexpr std::uint32_t secondaryButton  = 3U;
    constexpr std::uint32_t wheelUpButton    = 4U;
    constexpr std::uint32_t wheelDownButton  = 5U;
    constexpr std::uint32_t wheelLeftButton  = 6U;
    constexpr std::uint32_t wheelRightButton = 7U;

    constexpr std::size_t   oneGesture       = 1U;
    constexpr std::size_t   twoGestures      = 2U;
    constexpr std::size_t   firstGestureIndex{};
    constexpr std::size_t   secondGestureIndex = 1U;

    constexpr grab::CoordinateSpaceId firstSpace{ 7U };
    constexpr grab::CoordinateSpaceId secondSpace{ 8U };
    constexpr grab::SpacePoint        pressPoint{
        .x     = 100.0,
        .y     = 200.0,
        .space = firstSpace,
    };
    constexpr grab::SpacePoint withinSlopPoint{
        .x     = 102.0,
        .y     = 201.0,
        .space = firstSpace,
    };
    constexpr grab::SpacePoint slopBoundaryPoint{
        .x     = 103.0,
        .y     = 204.0,
        .space = firstSpace,
    };
    constexpr grab::SpacePoint outsideSlopPoint{
        .x     = 106.0,
        .y     = 200.0,
        .space = firstSpace,
    };
    constexpr grab::SpacePoint firstMotionPoint{
        .x     = 300.0,
        .y     = 400.0,
        .space = firstSpace,
    };
    constexpr grab::SpacePoint secondMotionPoint{
        .x     = 320.0,
        .y     = 420.0,
        .space = firstSpace,
    };
    constexpr grab::SpacePoint changedSpacePoint{
        .x     = pressPoint.x,
        .y     = pressPoint.y,
        .space = secondSpace,
    };

    constexpr gesture::GestureThresholds thresholds{
        .hold         = holdThreshold,
        .double_click = doubleClickThreshold,
        .pause        = pauseThreshold,
        .slop_px      = slopPx,
    };

    constexpr auto eligibleButtons =
        std::array{ primaryButton, middleButton, secondaryButton };
    constexpr auto wheelButtons = std::array{
        wheelUpButton,
        wheelDownButton,
        wheelLeftButton,
        wheelRightButton,
    };

    [[nodiscard]]
    constexpr double
    seconds( Milliseconds time ) noexcept
    {
        return std::chrono::duration<double>{ time }.count();
    }

    [[nodiscard]]
    grab::Event
    motion_event( Milliseconds                           time,
                  const std::optional<grab::SpacePoint>& position )
    {
        return grab::Event{
            .timestamp = seconds( time ),
            .kind      = grab::EventKind::MouseMove,
            .category  = grab::EventCategory::Input,
            .payload   = grab::MouseMove{
                                         .axis     = "x",
                                         .delta    = zeroDelta,
                                         .position = position,
                                         },
        };
    }

    [[nodiscard]]
    grab::Event
    button_event( grab::EventKind                        kind,
                  std::uint32_t                          button,
                  Milliseconds                           time,
                  const std::optional<grab::SpacePoint>& position )
    {
        return grab::Event{
            .timestamp = seconds( time ),
            .kind      = kind,
            .category  = grab::EventCategory::Input,
            .payload   = grab::MouseButton{
                                           .button   = button,
                                           .name     = {},
                                           .position = position,
                                           },
        };
    }

    [[nodiscard]]
    grab::Event
    motion( Milliseconds            time,
            const grab::SpacePoint& position )
    {
        return motion_event( time, position );
    }

    [[nodiscard]]
    grab::Event
    positionless_motion( Milliseconds time )
    {
        return motion_event( time, std::nullopt );
    }

    [[nodiscard]]
    grab::Event
    down( std::uint32_t           button,
          Milliseconds            time,
          const grab::SpacePoint& position )
    {
        return button_event( grab::EventKind::MouseButtonDown, button, time, position );
    }

    [[nodiscard]]
    grab::Event
    positionless_down( std::uint32_t button,
                       Milliseconds  time )
    {
        return button_event( grab::EventKind::MouseButtonDown,
                             button,
                             time,
                             std::nullopt );
    }

    [[nodiscard]]
    grab::Event
    up( std::uint32_t           button,
        Milliseconds            time,
        const grab::SpacePoint& position )
    {
        return button_event( grab::EventKind::MouseButtonUp, button, time, position );
    }

    [[nodiscard]]
    grab::Event
    positionless_up( std::uint32_t button,
                     Milliseconds  time )
    {
        return button_event( grab::EventKind::MouseButtonUp,
                             button,
                             time,
                             std::nullopt );
    }

    void
    expect_gesture( const gesture::GestureEvent& actual,
                    gesture::Gesture             kind,
                    const grab::SpacePoint&      position,
                    std::uint32_t                button )
    {
        EXPECT_EQ( actual.kind, kind );
        EXPECT_DOUBLE_EQ( actual.at.x, position.x );
        EXPECT_DOUBLE_EQ( actual.at.y, position.y );
        EXPECT_EQ( actual.at.space, position.space );
        EXPECT_EQ( actual.button, button );
    }

    void
    expect_single_gesture( const std::vector<gesture::GestureEvent>& actual,
                           gesture::Gesture                          kind,
                           const grab::SpacePoint&                   position,
                           std::uint32_t                             button )
    {
        ASSERT_EQ( actual.size(), oneGesture );
        expect_gesture( actual.at( firstGestureIndex ), kind, position, button );
    }

}    // namespace

TEST( GestureClassifier,
      IdleAndMovingMotionsEmitMoveThenPauseAtTheInclusiveDeadline )
{
    gesture::GestureClassifier classifier{ thresholds };

    expect_single_gesture( classifier.feed( motion( startTime, firstMotionPoint ) ),
                           gesture::Gesture::Move,
                           firstMotionPoint,
                           noButton );

    const auto secondMotionTime = startTime + eventStep;
    expect_single_gesture( classifier.feed( motion( secondMotionTime,
                                                    secondMotionPoint ) ),
                           gesture::Gesture::Move,
                           secondMotionPoint,
                           noButton );

    EXPECT_TRUE(
        classifier.advance( secondMotionTime + pauseThreshold - boundaryStep ).empty()
    );
    expect_single_gesture( classifier.advance( secondMotionTime + pauseThreshold ),
                           gesture::Gesture::Pause,
                           secondMotionPoint,
                           noButton );
    EXPECT_TRUE(
        classifier.advance( secondMotionTime + pauseThreshold + boundaryStep ).empty()
    );
}

TEST( GestureClassifier,
      MovingEligiblePressEntersPressedAndCanBecomeHold )
{
    gesture::GestureClassifier classifier{ thresholds };

    expect_single_gesture( classifier.feed( motion( startTime, firstMotionPoint ) ),
                           gesture::Gesture::Move,
                           firstMotionPoint,
                           noButton );
    const auto pressTime = startTime + eventStep;
    EXPECT_TRUE(
        classifier.feed( down( middleButton, pressTime, pressPoint ) ).empty()
    );
    expect_single_gesture( classifier.advance( pressTime + holdThreshold ),
                           gesture::Gesture::Hold,
                           pressPoint,
                           middleButton );
}

TEST( GestureClassifier,
      HoldFiresAtExactThresholdAndHoldingReleaseEmitsHoldEnd )
{
    gesture::GestureClassifier classifier{ thresholds };

    EXPECT_TRUE(
        classifier.feed( down( primaryButton, startTime, pressPoint ) ).empty()
    );
    EXPECT_TRUE( classifier.advance( startTime + beforeHold ).empty() );
    expect_single_gesture( classifier.advance( startTime + holdThreshold ),
                           gesture::Gesture::Hold,
                           pressPoint,
                           primaryButton );

    const auto smallMotionTime = startTime + holdThreshold + eventStep;
    EXPECT_TRUE( classifier.feed( motion( smallMotionTime, withinSlopPoint ) ).empty() );
    expect_single_gesture( classifier.feed( up( primaryButton,
                                                smallMotionTime + eventStep,
                                                withinSlopPoint ) ),
                           gesture::Gesture::HoldEnd,
                           withinSlopPoint,
                           primaryButton );
}

TEST( GestureClassifier,
      PressedMotionBeyondSlopCancelsPressAndEmitsMove )
{
    gesture::GestureClassifier classifier{ thresholds };

    EXPECT_TRUE(
        classifier.feed( down( primaryButton, startTime, pressPoint ) ).empty()
    );
    const auto motionTime = startTime + eventStep;
    expect_single_gesture( classifier.feed( motion( motionTime, outsideSlopPoint ) ),
                           gesture::Gesture::Move,
                           outsideSlopPoint,
                           noButton );
    EXPECT_TRUE(
        classifier.feed( up( primaryButton, motionTime + eventStep, outsideSlopPoint ) )
            .empty()
    );
    expect_single_gesture( classifier.advance( motionTime + pauseThreshold ),
                           gesture::Gesture::Pause,
                           outsideSlopPoint,
                           noButton );
}

TEST( GestureClassifier,
      PressedReleaseBeyondSlopCancelsWithoutClick )
{
    gesture::GestureClassifier classifier{ thresholds };

    EXPECT_TRUE(
        classifier.feed( down( primaryButton, startTime, pressPoint ) ).empty()
    );
    EXPECT_TRUE(
        classifier.feed( up( primaryButton, startTime + eventStep, outsideSlopPoint ) )
            .empty()
    );
    EXPECT_TRUE( classifier.advance( startTime + longAdvance ).empty() );
    expect_single_gesture( classifier.feed( motion( startTime + longAdvance + eventStep,
                                                    firstMotionPoint ) ),
                           gesture::Gesture::Move,
                           firstMotionPoint,
                           noButton );
}

TEST( GestureClassifier,
      PressedReleaseDefersClickUntilInclusiveDoubleWindowExpires )
{
    gesture::GestureClassifier classifier{ thresholds };

    EXPECT_TRUE(
        classifier.feed( down( primaryButton, startTime, pressPoint ) ).empty()
    );
    EXPECT_TRUE( classifier
                     .feed( up( primaryButton, startTime + eventStep, withinSlopPoint ) )
                     .empty() );
    EXPECT_TRUE( classifier.advance( startTime + doubleClickThreshold ).empty() );
    expect_single_gesture( classifier.advance( startTime + afterDoubleClick ),
                           gesture::Gesture::Click,
                           pressPoint,
                           primaryButton );
}

TEST( GestureClassifier,
      HoldingMotionBeyondSlopEmitsHoldCancelThenMove )
{
    gesture::GestureClassifier classifier{ thresholds };

    EXPECT_TRUE(
        classifier.feed( down( primaryButton, startTime, pressPoint ) ).empty()
    );
    expect_single_gesture( classifier.advance( startTime + holdThreshold ),
                           gesture::Gesture::Hold,
                           pressPoint,
                           primaryButton );

    const auto motionTime = startTime + holdThreshold + eventStep;
    const auto output     = classifier.feed( motion( motionTime, outsideSlopPoint ) );
    ASSERT_EQ( output.size(), twoGestures );
    expect_gesture( output.at( firstGestureIndex ),
                    gesture::Gesture::HoldCancel,
                    outsideSlopPoint,
                    primaryButton );
    expect_gesture( output.at( secondGestureIndex ),
                    gesture::Gesture::Move,
                    outsideSlopPoint,
                    noButton );

    EXPECT_TRUE(
        classifier.feed( up( primaryButton, motionTime + eventStep, outsideSlopPoint ) )
            .empty()
    );
    expect_single_gesture( classifier.advance( motionTime + pauseThreshold ),
                           gesture::Gesture::Pause,
                           outsideSlopPoint,
                           noButton );
}

TEST( GestureClassifier,
      ExactBoundarySecondPressEmitsDoubleClickAndNoTrailingClick )
{
    gesture::GestureClassifier classifier{ thresholds };

    EXPECT_TRUE(
        classifier.feed( down( primaryButton, startTime, pressPoint ) ).empty()
    );
    EXPECT_TRUE( classifier
                     .feed( up( primaryButton, startTime + eventStep, withinSlopPoint ) )
                     .empty() );
    expect_single_gesture( classifier.feed( down( primaryButton,
                                                  startTime + doubleClickThreshold,
                                                  slopBoundaryPoint ) ),
                           gesture::Gesture::DoubleClick,
                           slopBoundaryPoint,
                           primaryButton );
    EXPECT_TRUE( classifier
                     .feed( up( primaryButton,
                                startTime + doubleClickThreshold + eventStep,
                                slopBoundaryPoint ) )
                     .empty() );
    EXPECT_TRUE( classifier.advance( startTime + longAdvance ).empty() );
}

TEST( GestureClassifier,
      DifferentButtonWhileAwaitingDoubleIsIgnoredAndTracked )
{
    gesture::GestureClassifier classifier{ thresholds };

    EXPECT_TRUE(
        classifier.feed( down( primaryButton, startTime, pressPoint ) ).empty()
    );
    EXPECT_TRUE( classifier
                     .feed( up( primaryButton, startTime + eventStep, withinSlopPoint ) )
                     .empty() );
    const auto otherPressTime = startTime + ( eventStep + eventStep );
    EXPECT_TRUE(
        classifier.feed( down( middleButton, otherPressTime, outsideSlopPoint ) ).empty()
    );
    EXPECT_TRUE(
        classifier
            .feed( up( middleButton, otherPressTime + boundaryStep, outsideSlopPoint ) )
            .empty()
    );

    const auto secondPrimaryPressTime = startTime + insideDoubleClick;
    expect_single_gesture( classifier.feed( down( primaryButton,
                                                  secondPrimaryPressTime,
                                                  withinSlopPoint ) ),
                           gesture::Gesture::DoubleClick,
                           withinSlopPoint,
                           primaryButton );
    EXPECT_TRUE( classifier
                     .feed( up( primaryButton,
                                secondPrimaryPressTime + eventStep,
                                withinSlopPoint ) )
                     .empty() );
    EXPECT_TRUE( classifier.advance( startTime + longAdvance ).empty() );
}

TEST( GestureClassifier,
      ThirdPressAfterDoubleClickStartsFreshSingleClickCycle )
{
    gesture::GestureClassifier classifier{ thresholds };

    EXPECT_TRUE(
        classifier.feed( down( primaryButton, startTime, pressPoint ) ).empty()
    );
    EXPECT_TRUE( classifier
                     .feed( up( primaryButton, startTime + eventStep, withinSlopPoint ) )
                     .empty() );
    const auto secondPressTime = startTime + insideDoubleClick;
    expect_single_gesture(
        classifier.feed( down( primaryButton, secondPressTime, withinSlopPoint ) ),
        gesture::Gesture::DoubleClick,
        withinSlopPoint,
        primaryButton
    );
    EXPECT_TRUE(
        classifier
            .feed( up( primaryButton, secondPressTime + eventStep, withinSlopPoint ) )
            .empty()
    );

    const auto thirdPressTime = startTime + longAdvance;
    EXPECT_TRUE( classifier
                     .feed( down( primaryButton, thirdPressTime, outsideSlopPoint ) )
                     .empty() );
    EXPECT_TRUE(
        classifier
            .feed( up( primaryButton, thirdPressTime + eventStep, outsideSlopPoint ) )
            .empty()
    );
    expect_single_gesture( classifier.advance( thirdPressTime + afterDoubleClick ),
                           gesture::Gesture::Click,
                           outsideSlopPoint,
                           primaryButton );
}

TEST( GestureClassifier,
      MotionAfterDoubleClickStartsMoveWithoutTrailingClick )
{
    gesture::GestureClassifier classifier{ thresholds };

    EXPECT_TRUE(
        classifier.feed( down( primaryButton, startTime, pressPoint ) ).empty()
    );
    EXPECT_TRUE( classifier
                     .feed( up( primaryButton, startTime + eventStep, withinSlopPoint ) )
                     .empty() );
    const auto secondPressTime = startTime + insideDoubleClick;
    expect_single_gesture(
        classifier.feed( down( primaryButton, secondPressTime, withinSlopPoint ) ),
        gesture::Gesture::DoubleClick,
        withinSlopPoint,
        primaryButton
    );
    EXPECT_TRUE(
        classifier
            .feed( up( primaryButton, secondPressTime + eventStep, withinSlopPoint ) )
            .empty()
    );

    const auto motionTime = secondPressTime + ( eventStep + eventStep );
    expect_single_gesture( classifier.feed( motion( motionTime, firstMotionPoint ) ),
                           gesture::Gesture::Move,
                           firstMotionPoint,
                           noButton );
    expect_single_gesture( classifier.advance( motionTime + pauseThreshold ),
                           gesture::Gesture::Pause,
                           firstMotionPoint,
                           noButton );
}

TEST( GestureClassifier,
      AwaitDoubleMotionBeyondSlopEmitsClickThenMove )
{
    gesture::GestureClassifier classifier{ thresholds };

    EXPECT_TRUE(
        classifier.feed( down( primaryButton, startTime, pressPoint ) ).empty()
    );
    EXPECT_TRUE( classifier
                     .feed( up( primaryButton, startTime + eventStep, withinSlopPoint ) )
                     .empty() );
    const auto motionTime = startTime + insideDoubleClick;
    const auto output     = classifier.feed( motion( motionTime, outsideSlopPoint ) );
    ASSERT_EQ( output.size(), twoGestures );
    expect_gesture( output.at( firstGestureIndex ),
                    gesture::Gesture::Click,
                    pressPoint,
                    primaryButton );
    expect_gesture( output.at( secondGestureIndex ),
                    gesture::Gesture::Move,
                    outsideSlopPoint,
                    noButton );
}

TEST( GestureClassifier,
      PressesJustOutsideDoubleWindowBecomeSeparateClicks )
{
    gesture::GestureClassifier classifier{ thresholds };

    EXPECT_TRUE(
        classifier.feed( down( primaryButton, startTime, pressPoint ) ).empty()
    );
    EXPECT_TRUE( classifier
                     .feed( up( primaryButton, startTime + eventStep, withinSlopPoint ) )
                     .empty() );

    const auto secondPressTime = startTime + afterDoubleClick;
    expect_single_gesture(
        classifier.feed( down( primaryButton, secondPressTime, withinSlopPoint ) ),
        gesture::Gesture::Click,
        pressPoint,
        primaryButton
    );
    EXPECT_TRUE(
        classifier
            .feed( up( primaryButton, secondPressTime + eventStep, withinSlopPoint ) )
            .empty()
    );
    expect_single_gesture( classifier.advance( secondPressTime + afterDoubleClick ),
                           gesture::Gesture::Click,
                           withinSlopPoint,
                           primaryButton );
}

TEST( GestureClassifier,
      SameButtonPressWithinWindowBeyondSlopStartsSeparateClickCycle )
{
    gesture::GestureClassifier classifier{ thresholds };

    EXPECT_TRUE(
        classifier.feed( down( primaryButton, startTime, pressPoint ) ).empty()
    );
    EXPECT_TRUE( classifier
                     .feed( up( primaryButton, startTime + eventStep, withinSlopPoint ) )
                     .empty() );

    const auto secondPressTime = startTime + insideDoubleClick;
    expect_single_gesture(
        classifier.feed( down( primaryButton, secondPressTime, outsideSlopPoint ) ),
        gesture::Gesture::Click,
        pressPoint,
        primaryButton
    );
    EXPECT_TRUE(
        classifier
            .feed( up( primaryButton, secondPressTime + eventStep, outsideSlopPoint ) )
            .empty()
    );
    expect_single_gesture( classifier.advance( secondPressTime + afterDoubleClick ),
                           gesture::Gesture::Click,
                           outsideSlopPoint,
                           primaryButton );
}

TEST( GestureClassifier,
      ReleaseAtHoldDeadlineEmitsHoldBeforeHoldEnd )
{
    gesture::GestureClassifier classifier{ thresholds };

    EXPECT_TRUE(
        classifier.feed( down( primaryButton, startTime, pressPoint ) ).empty()
    );
    const auto output = classifier.feed(
        up( primaryButton, startTime + holdThreshold, withinSlopPoint )
    );
    ASSERT_EQ( output.size(), twoGestures );
    expect_gesture( output.at( firstGestureIndex ),
                    gesture::Gesture::Hold,
                    pressPoint,
                    primaryButton );
    expect_gesture( output.at( secondGestureIndex ),
                    gesture::Gesture::HoldEnd,
                    withinSlopPoint,
                    primaryButton );
}

TEST( GestureClassifier,
      WheelButtonsNeverProduceGestures )
{
    for( const auto button : wheelButtons )
    {
        gesture::GestureClassifier classifier{ thresholds };
        EXPECT_TRUE( classifier.feed( down( button, startTime, pressPoint ) ).empty() );
        EXPECT_TRUE(
            classifier.feed( up( button, startTime + eventStep, pressPoint ) ).empty()
        );
        EXPECT_TRUE( classifier.advance( startTime + longAdvance ).empty() );
    }
}

TEST( GestureClassifier,
      EveryEligibleButtonCanProduceClick )
{
    for( const auto button : eligibleButtons )
    {
        gesture::GestureClassifier classifier{ thresholds };
        EXPECT_TRUE( classifier.feed( down( button, startTime, pressPoint ) ).empty() );
        EXPECT_TRUE(
            classifier.feed( up( button, startTime + eventStep, pressPoint ) ).empty()
        );
        expect_single_gesture( classifier.advance( startTime + afterDoubleClick ),
                               gesture::Gesture::Click,
                               pressPoint,
                               button );
    }
}

TEST( GestureClassifier,
      OtherButtonMidGestureAndItsReleaseDoNotStealOwnership )
{
    gesture::GestureClassifier classifier{ thresholds };

    EXPECT_TRUE(
        classifier.feed( down( primaryButton, startTime, pressPoint ) ).empty()
    );
    EXPECT_TRUE(
        classifier
            .feed( down( secondaryButton, startTime + eventStep, outsideSlopPoint ) )
            .empty()
    );
    EXPECT_TRUE( classifier
                     .feed( up( secondaryButton,
                                startTime + ( eventStep + eventStep ),
                                outsideSlopPoint ) )
                     .empty() );
    expect_single_gesture( classifier.advance( startTime + holdThreshold ),
                           gesture::Gesture::Hold,
                           pressPoint,
                           primaryButton );
    expect_single_gesture( classifier.feed( up( primaryButton,
                                                startTime + holdThreshold + eventStep,
                                                withinSlopPoint ) ),
                           gesture::Gesture::HoldEnd,
                           withinSlopPoint,
                           primaryButton );

    const auto nextPressTime = startTime + holdThreshold + ( eventStep + eventStep );
    EXPECT_TRUE( classifier
                     .feed( down( secondaryButton, nextPressTime, outsideSlopPoint ) )
                     .empty() );
    EXPECT_TRUE(
        classifier
            .feed( up( secondaryButton, nextPressTime + eventStep, outsideSlopPoint ) )
            .empty()
    );
    expect_single_gesture( classifier.advance( nextPressTime + afterDoubleClick ),
                           gesture::Gesture::Click,
                           outsideSlopPoint,
                           secondaryButton );
}

TEST( GestureClassifier,
      ExactSlopBoundaryRemainsPressedAndSpaceChangeExceedsSlop )
{
    gesture::GestureClassifier classifier{ thresholds };

    EXPECT_TRUE(
        classifier.feed( down( primaryButton, startTime, pressPoint ) ).empty()
    );
    EXPECT_TRUE(
        classifier.feed( motion( startTime + eventStep, slopBoundaryPoint ) ).empty()
    );
    EXPECT_TRUE( classifier
                     .feed( up( primaryButton,
                                startTime + ( eventStep + eventStep ),
                                slopBoundaryPoint ) )
                     .empty() );
    expect_single_gesture( classifier.advance( startTime + afterDoubleClick ),
                           gesture::Gesture::Click,
                           pressPoint,
                           primaryButton );

    const auto changedSpacePressTime = startTime + longAdvance;
    EXPECT_TRUE( classifier
                     .feed( down( primaryButton, changedSpacePressTime, pressPoint ) )
                     .empty() );
    expect_single_gesture( classifier.feed( motion( changedSpacePressTime + eventStep,
                                                    changedSpacePoint ) ),
                           gesture::Gesture::Move,
                           changedSpacePoint,
                           noButton );
}

TEST( GestureClassifier,
      PositionlessEventsPreserveSpatialAnchorsAndDoNotResetPause )
{
    gesture::GestureClassifier classifier{ thresholds };

    EXPECT_TRUE(
        classifier.feed( positionless_down( primaryButton, startTime ) ).empty()
    );
    EXPECT_TRUE( classifier
                     .feed( positionless_up( primaryButton, startTime + eventStep ) )
                     .empty() );

    const auto pressTime = startTime + ( eventStep + eventStep );
    EXPECT_TRUE(
        classifier.feed( down( primaryButton, pressTime, pressPoint ) ).empty()
    );
    EXPECT_TRUE(
        classifier.feed( positionless_motion( pressTime + eventStep ) ).empty()
    );
    expect_single_gesture( classifier.advance( pressTime + holdThreshold ),
                           gesture::Gesture::Hold,
                           pressPoint,
                           primaryButton );
    expect_single_gesture(
        classifier.feed( positionless_up( primaryButton,
                                          pressTime + holdThreshold + eventStep ) ),
        gesture::Gesture::HoldEnd,
        pressPoint,
        primaryButton
    );

    const auto motionTime = pressTime + longAdvance;
    expect_single_gesture( classifier.feed( motion( motionTime, firstMotionPoint ) ),
                           gesture::Gesture::Move,
                           firstMotionPoint,
                           noButton );
    EXPECT_TRUE(
        classifier.feed( positionless_motion( motionTime + eventStep ) ).empty()
    );
    expect_single_gesture( classifier.advance( motionTime + pauseThreshold ),
                           gesture::Gesture::Pause,
                           firstMotionPoint,
                           noButton );
}

TEST( GestureClassifier,
      PositionlessFirstPressRetainsOwnershipUntilItsRelease )
{
    gesture::GestureClassifier classifier{ thresholds };

    EXPECT_TRUE(
        classifier.feed( positionless_down( primaryButton, startTime ) ).empty()
    );
    const auto otherPressTime = startTime + eventStep;
    EXPECT_TRUE(
        classifier.feed( down( middleButton, otherPressTime, outsideSlopPoint ) ).empty()
    );
    EXPECT_TRUE( classifier.advance( otherPressTime + holdThreshold ).empty() );
    EXPECT_TRUE( classifier
                     .feed( up( middleButton,
                                otherPressTime + holdThreshold + boundaryStep,
                                outsideSlopPoint ) )
                     .empty() );
    const auto ownerReleaseTime = otherPressTime + holdThreshold + eventStep;
    EXPECT_TRUE(
        classifier.feed( positionless_up( primaryButton, ownerReleaseTime ) ).empty()
    );

    const auto freshPressTime = ownerReleaseTime + eventStep;
    EXPECT_TRUE(
        classifier.feed( down( middleButton, freshPressTime, outsideSlopPoint ) ).empty()
    );
    EXPECT_TRUE(
        classifier
            .feed( up( middleButton, freshPressTime + eventStep, outsideSlopPoint ) )
            .empty()
    );
    expect_single_gesture( classifier.advance( freshPressTime + afterDoubleClick ),
                           gesture::Gesture::Click,
                           outsideSlopPoint,
                           middleButton );
}

TEST( GestureClassifier,
      PositionlessPressedReleaseDefersClickAtStoredPressPoint )
{
    gesture::GestureClassifier classifier{ thresholds };

    EXPECT_TRUE(
        classifier.feed( down( primaryButton, startTime, pressPoint ) ).empty()
    );
    EXPECT_TRUE( classifier
                     .feed( positionless_up( primaryButton, startTime + eventStep ) )
                     .empty() );
    expect_single_gesture( classifier.advance( startTime + afterDoubleClick ),
                           gesture::Gesture::Click,
                           pressPoint,
                           primaryButton );
}

TEST( GestureClassifier,
      PauseDeadlinePrecedesMotionAtTheSameTimestamp )
{
    gesture::GestureClassifier classifier{ thresholds };

    expect_single_gesture( classifier.feed( motion( startTime, firstMotionPoint ) ),
                           gesture::Gesture::Move,
                           firstMotionPoint,
                           noButton );
    const auto output =
        classifier.feed( motion( startTime + pauseThreshold, secondMotionPoint ) );
    ASSERT_EQ( output.size(), twoGestures );
    expect_gesture( output.at( firstGestureIndex ),
                    gesture::Gesture::Pause,
                    firstMotionPoint,
                    noButton );
    expect_gesture( output.at( secondGestureIndex ),
                    gesture::Gesture::Move,
                    secondMotionPoint,
                    noButton );
}

TEST( GestureClassifier,
      ResetReturnsToIdleAndDropsEveryPendingDeadline )
{
    gesture::GestureClassifier classifier{ thresholds };

    EXPECT_TRUE(
        classifier.feed( down( primaryButton, startTime, pressPoint ) ).empty()
    );
    classifier.reset();
    EXPECT_TRUE(
        classifier.advance( startTime + holdThreshold + boundaryStep ).empty()
    );

    const auto motionTime = startTime + longAdvance;
    expect_single_gesture( classifier.feed( motion( motionTime, firstMotionPoint ) ),
                           gesture::Gesture::Move,
                           firstMotionPoint,
                           noButton );
    classifier.reset();
    EXPECT_TRUE(
        classifier.advance( motionTime + pauseThreshold + boundaryStep ).empty()
    );

    const auto clickPressTime = motionTime + longAdvance;
    EXPECT_TRUE(
        classifier.feed( down( primaryButton, clickPressTime, pressPoint ) ).empty()
    );
    EXPECT_TRUE(
        classifier
            .feed( up( primaryButton, clickPressTime + eventStep, withinSlopPoint ) )
            .empty()
    );
    classifier.reset();
    EXPECT_TRUE( classifier.advance( clickPressTime + afterDoubleClick ).empty() );

    const auto finalMotionTime = clickPressTime + longAdvance;
    expect_single_gesture( classifier.feed( motion( finalMotionTime,
                                                    secondMotionPoint ) ),
                           gesture::Gesture::Move,
                           secondMotionPoint,
                           noButton );
}
