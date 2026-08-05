#include "grab/event.hpp"
#include "grab/overlay.hpp"
#include "grab/result.hpp"
#include "grab/session.hpp"
#include "grab/space.hpp"
#include "kernel/events/event_bus.hpp"
#include "kernel/input/gesture_classifier.hpp"
#include "kernel/presentation/cursor_feedback.hpp"
#include "kernel/presentation/overlay_scene.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
// clang-format on

namespace
{

    namespace input        = grab::kernel::input;
    namespace presentation = grab::kernel::presentation;

    using Milliseconds     = std::chrono::milliseconds;

    constexpr Milliseconds  sceneTime{ 100 };
    constexpr Milliseconds  rippleDuration{ 400 };
    constexpr Milliseconds  holdThreshold{ 500 };
    constexpr Milliseconds  doubleClickThreshold{ 400 };
    constexpr Milliseconds  pauseThreshold{ 700 };
    constexpr Milliseconds  observerClockTime{ 1'000 };
    constexpr Milliseconds  startupFirstPressTime{ 1'000 };
    constexpr Milliseconds  startupFirstReleaseTime{ 1'010 };
    constexpr Milliseconds  startupSecondPressTime{ 1'020 };

    constexpr std::size_t   noShapes              = 0U;
    constexpr std::size_t   oneShape              = 1U;
    constexpr std::size_t   twoShapes             = 2U;
    constexpr std::size_t   firstShapeIndex       = 0U;
    constexpr std::size_t   noAddCalls            = 0U;

    constexpr std::uint32_t primaryButton         = 1U;
    constexpr std::uint8_t  rippleRed             = 17U;
    constexpr std::uint8_t  rippleGreen           = 34U;
    constexpr std::uint8_t  rippleBlue            = 51U;
    constexpr std::uint8_t  rippleAlpha           = 221U;
    constexpr std::uint8_t  progressRed           = 68U;
    constexpr std::uint8_t  progressGreen         = 85U;
    constexpr std::uint8_t  progressBlue          = 102U;
    constexpr std::uint8_t  progressAlpha         = 238U;

    constexpr double        hiddenFraction        = 0.0;
    constexpr double        visibleFraction       = 1.0;
    constexpr double        rippleRadiusPx        = 48.0;
    constexpr double        progressWidthPx       = 64.0;
    constexpr double        progressHeightPx      = 6.0;
    constexpr double        progressOffsetYPx     = 24.0;
    constexpr double        progressCenterDivisor = 2.0;
    constexpr double        gestureSlopPx         = 5.0;
    constexpr double        cursorX               = 120.0;
    constexpr double        cursorY               = 240.0;
    constexpr double        cancelCursorX         = 180.0;
    constexpr double        cancelCursorY         = 260.0;
    constexpr double        expectedProgressX =
        cursorX - ( progressWidthPx / progressCenterDivisor );
    constexpr double                  expectedProgressY = cursorY + progressOffsetYPx;

    constexpr grab::CoordinateSpaceId feedbackSpace{ 7U };
    constexpr grab::SpacePoint        cursorPoint{
        .x     = cursorX,
        .y     = cursorY,
        .space = feedbackSpace,
    };
    constexpr grab::SpacePoint cancelPoint{
        .x     = cancelCursorX,
        .y     = cancelCursorY,
        .space = feedbackSpace,
    };

    constexpr grab::overlay::Color rippleColor{
        .r = rippleRed,
        .g = rippleGreen,
        .b = rippleBlue,
        .a = rippleAlpha,
    };
    constexpr grab::overlay::Color progressColor{
        .r = progressRed,
        .g = progressGreen,
        .b = progressBlue,
        .a = progressAlpha,
    };

    constexpr grab::CursorFeedbackConfig feedbackConfig{
        .click =
            grab::RippleStyle{
                              .radius_px = rippleRadiusPx,
                              .color     = rippleColor,
                              .duration  = rippleDuration,
                              },
        .hold =
            grab::ProgressStyle{
                              .width_px    = progressWidthPx,
                              .height_px   = progressHeightPx,
                              .offset_y_px = progressOffsetYPx,
                              .color       = progressColor,
                              },
        .thresholds = grab::GestureThresholds{
                              .hold         = holdThreshold,
                              .double_click = doubleClickThreshold,
                              .pause        = pauseThreshold,
                              .slop_px      = gestureSlopPx,
                              },
    };

    constexpr input::GestureEvent clickGesture{
        .kind   = input::Gesture::Click,
        .at     = cursorPoint,
        .button = primaryButton,
    };
    constexpr input::GestureEvent doubleClickGesture{
        .kind   = input::Gesture::DoubleClick,
        .at     = cursorPoint,
        .button = primaryButton,
    };
    constexpr input::GestureEvent holdGesture{
        .kind   = input::Gesture::Hold,
        .at     = cursorPoint,
        .button = primaryButton,
    };
    constexpr input::GestureEvent holdEndGesture{
        .kind   = input::Gesture::HoldEnd,
        .at     = cursorPoint,
        .button = primaryButton,
    };
    constexpr input::GestureEvent holdCancelGesture{
        .kind   = input::Gesture::HoldCancel,
        .at     = cancelPoint,
        .button = primaryButton,
    };

    constexpr std::string_view schedulingFailureMessage{
        "injected cursor feedback scheduling failure"
    };

    [[nodiscard]]
    presentation::OverlayScene
    feedback_scene()
    {
        return presentation::OverlayScene{ []
                                           {
                                               return sceneTime;
                                           } };
    }

    [[nodiscard]]
    grab::Event
    button_event( grab::EventKind kind,
                  Milliseconds    time )
    {
        return grab::Event{
            .timestamp = std::chrono::duration<double>{ time }
              .count(),
            .kind      = kind,
            .category  = grab::EventCategory::Input,
            .payload   = grab::MouseButton{
                                                       .button   = primaryButton,
                                                       .name     = {},
                                                       .position = cursorPoint,
                                                       },
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

}    // namespace

TEST( CursorFeedback,
      DefaultStylesUseDefaultOverlayColor )
{
    constexpr grab::RippleStyle   ripple{};
    constexpr grab::ProgressStyle progress{};

    expect_color( ripple.color, grab::overlay::defaultOverlayColor );
    expect_color( progress.color, grab::overlay::defaultOverlayColor );
}

TEST( CursorFeedback,
      ClickAddsOneRippleWithScaleAndOpacityChannels )
{
    auto                                  scene = feedback_scene();
    presentation::CursorFeedbackPresenter presenter{ scene, feedbackConfig };

    const auto                            consumed = presenter.consume( clickGesture );
    ASSERT_TRUE( consumed.has_value() ) << consumed.error().message;

    const auto snapshot = scene.snapshot();
    ASSERT_EQ( snapshot.shapes.size(), oneShape );
    const auto&       shape   = snapshot.shapes.at( firstShapeIndex ).shape;

    const auto* const ellipse = std::get_if<grab::overlay::Ellipse>( &shape.geometry );
    ASSERT_NE( ellipse, nullptr );
    EXPECT_DOUBLE_EQ( ellipse->center.x, cursorPoint.x );
    EXPECT_DOUBLE_EQ( ellipse->center.y, cursorPoint.y );
    EXPECT_EQ( ellipse->center.space, cursorPoint.space );
    EXPECT_DOUBLE_EQ( ellipse->radius_x, rippleRadiusPx );
    EXPECT_DOUBLE_EQ( ellipse->radius_y, rippleRadiusPx );
    ASSERT_TRUE( shape.fill.has_value() );
    expect_color( shape.fill->color, rippleColor );

    const auto* const lifetime = std::get_if<grab::overlay::Ttl>( &shape.lifetime );
    ASSERT_NE( lifetime, nullptr );
    EXPECT_EQ( lifetime->duration, rippleDuration );

    ASSERT_TRUE( shape.animation.has_value() );
    const auto& animation = *shape.animation;
    ASSERT_TRUE( animation.scale.has_value() );
    EXPECT_EQ( animation.scale->easing, grab::overlay::Easing::OutCubic );
    EXPECT_EQ( animation.scale->duration, rippleDuration );
    EXPECT_DOUBLE_EQ( animation.scale->from, hiddenFraction );
    EXPECT_DOUBLE_EQ( animation.scale->to, visibleFraction );
    ASSERT_TRUE( animation.opacity.has_value() );
    EXPECT_EQ( animation.opacity->duration, rippleDuration );
    EXPECT_DOUBLE_EQ( animation.opacity->from, visibleFraction );
    EXPECT_DOUBLE_EQ( animation.opacity->to, hiddenFraction );
    EXPECT_FALSE( animation.translate.has_value() );
    EXPECT_FALSE( animation.reveal.has_value() );
}

TEST( CursorFeedback,
      HoldAddsProgressBarWithThresholdDurationRevealChannel )
{
    auto                                  scene = feedback_scene();
    presentation::CursorFeedbackPresenter presenter{ scene, feedbackConfig };

    const auto                            consumed = presenter.consume( holdGesture );
    ASSERT_TRUE( consumed.has_value() ) << consumed.error().message;

    const auto snapshot = scene.snapshot();
    ASSERT_EQ( snapshot.shapes.size(), oneShape );
    const auto&       shape = snapshot.shapes.at( firstShapeIndex ).shape;

    const auto* const rect  = std::get_if<grab::overlay::Rect>( &shape.geometry );
    ASSERT_NE( rect, nullptr );
    EXPECT_DOUBLE_EQ( rect->bounds.x, expectedProgressX );
    EXPECT_DOUBLE_EQ( rect->bounds.y, expectedProgressY );
    EXPECT_DOUBLE_EQ( rect->bounds.w, progressWidthPx );
    EXPECT_DOUBLE_EQ( rect->bounds.h, progressHeightPx );
    EXPECT_EQ( rect->bounds.space, cursorPoint.space );
    ASSERT_TRUE( shape.fill.has_value() );
    expect_color( shape.fill->color, progressColor );
    EXPECT_TRUE( std::holds_alternative<grab::overlay::Persistent>( shape.lifetime ) );

    ASSERT_TRUE( shape.animation.has_value() );
    const auto& animation = *shape.animation;
    ASSERT_TRUE( animation.reveal.has_value() );
    EXPECT_EQ( animation.reveal->duration, holdThreshold );
    EXPECT_EQ( animation.reveal->axis, grab::overlay::Axis::X );
    EXPECT_EQ( animation.reveal->from_edge, grab::overlay::Edge::Min );
    EXPECT_DOUBLE_EQ( animation.reveal->from, hiddenFraction );
    EXPECT_DOUBLE_EQ( animation.reveal->to, visibleFraction );
    EXPECT_FALSE( animation.scale.has_value() );
    EXPECT_FALSE( animation.opacity.has_value() );
    EXPECT_FALSE( animation.translate.has_value() );
}

TEST( CursorFeedback,
      HoldEndAndHoldCancelEachRemoveTheProgressBar )
{
    auto                                  scene = feedback_scene();
    presentation::CursorFeedbackPresenter presenter{ scene, feedbackConfig };

    ASSERT_TRUE( presenter.consume( holdGesture ).has_value() );
    ASSERT_EQ( scene.snapshot().shapes.size(), oneShape );
    ASSERT_TRUE( presenter.consume( holdEndGesture ).has_value() );
    EXPECT_EQ( scene.snapshot().shapes.size(), noShapes );

    ASSERT_TRUE( presenter.consume( holdGesture ).has_value() );
    ASSERT_EQ( scene.snapshot().shapes.size(), oneShape );
    ASSERT_TRUE( presenter.consume( holdCancelGesture ).has_value() );
    EXPECT_EQ( scene.snapshot().shapes.size(), noShapes );
}

TEST( CursorFeedback,
      DoubleClickAddsRippleAndRemovesStaleProgressBar )
{
    auto                                  scene = feedback_scene();
    presentation::CursorFeedbackPresenter presenter{ scene, feedbackConfig };

    ASSERT_TRUE( presenter.consume( holdGesture ).has_value() );
    ASSERT_EQ( scene.snapshot().shapes.size(), oneShape );
    ASSERT_TRUE( presenter.consume( doubleClickGesture ).has_value() );

    const auto snapshot = scene.snapshot();
    ASSERT_EQ( snapshot.shapes.size(), oneShape );
    EXPECT_TRUE( std::holds_alternative<grab::overlay::Ellipse>(
        snapshot.shapes.at( firstShapeIndex ).shape.geometry
    ) );
}

TEST( CursorFeedback,
      PresenterTeardownRemovesEveryOwnedShape )
{
    auto scene = feedback_scene();
    {
        presentation::CursorFeedbackPresenter presenter{ scene, feedbackConfig };
        ASSERT_TRUE( presenter.consume( clickGesture ).has_value() );
        ASSERT_TRUE( presenter.consume( holdGesture ).has_value() );
        ASSERT_EQ( scene.snapshot().shapes.size(), twoShapes );
    }

    EXPECT_EQ( scene.snapshot().shapes.size(), noShapes );
}

TEST( CursorFeedback,
      ObserverSchedulingFailureRollsBackShapesAndAllInputDemand )
{
    grab::EventBus                            bus;
    auto                                      scene = feedback_scene();
    std::size_t                               add_calls{};

    presentation::CursorFeedbackObserverHooks hooks{
        .bus  = &bus,
        .post = []( std::function<void()> callback ) -> grab::Result<void>
        {
            callback();
            return {};
        },
        .schedule = [&bus]( std::chrono::nanoseconds,
                            std::function<void()> ) -> grab::Result<void>
        {
            bus.publish( button_event( grab::EventKind::MouseButtonDown,
                                       startupFirstPressTime ) );
            bus.publish( button_event( grab::EventKind::MouseButtonUp,
                                       startupFirstReleaseTime ) );
            bus.publish( button_event( grab::EventKind::MouseButtonDown,
                                       startupSecondPressTime ) );
            return grab::fail( grab::ErrorCode::InternalFault,
                               std::string{ schedulingFailureMessage } );
        },
        .clock =
            []
        {
            return observerClockTime;
        },
        .add_shape =
            [&scene, &add_calls]( grab::overlay::Shape shape )
        {
            ++add_calls;
            return scene.add( std::move( shape ) );
        },
        .remove_shape =
            [&scene]( grab::overlay::ShapeId id )
        {
            return scene.remove( id );
        },
        .on_reactor_thread =
            []
        {
            return true;
        },
        .reactor_alive =
            []
        {
            return true;
        },
        .release_observation = {},
    };

    const auto observer =
        presentation::CursorFeedbackObserver::start( feedbackConfig,
                                                     std::move( hooks ) );

    ASSERT_FALSE( observer.has_value() );
    EXPECT_EQ( observer.error().code, grab::ErrorCode::InternalFault );
    EXPECT_EQ( add_calls, noAddCalls );
    EXPECT_EQ( scene.snapshot().shapes.size(), noShapes );
    EXPECT_EQ( bus.subscription_refcount( grab::EventKind::MouseMove ), noShapes );
    EXPECT_EQ( bus.subscription_refcount( grab::EventKind::MouseButtonDown ), noShapes );
    EXPECT_EQ( bus.subscription_refcount( grab::EventKind::MouseButtonUp ), noShapes );
}
