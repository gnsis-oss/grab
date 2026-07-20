#include "config/schedule.hpp"
#include "grab/config.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
// clang-format on

namespace
{

    using Clock     = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    constexpr TimePoint                          origin{};
    constexpr std::chrono::milliseconds          captureInterval{ 100 };
    constexpr std::chrono::milliseconds          slowCaptureDuration{ 75 };
    constexpr std::chrono::milliseconds          interleavedDelay{ 150 };
    constexpr std::chrono::milliseconds          overrunDuration{ 250 };
    constexpr std::chrono::milliseconds          noStepDelay{};
    constexpr std::size_t                        noStepIndex     = 0U;
    constexpr std::size_t                        firstStepIndex  = 0U;
    constexpr std::size_t                        secondStepIndex = 1U;
    constexpr std::size_t                        thirdStepIndex  = 2U;
    constexpr std::uint64_t                      noSkipped       = 0U;
    constexpr std::uint64_t                      oneSkipped      = 1U;
    constexpr bool                               looping         = true;
    constexpr bool                               singlePass      = false;
    constexpr const grab::config::ScriptSection* noScript        = nullptr;

    [[nodiscard]]
    grab::config::ScriptStep
    make_step( grab::config::StepAction  action,
               std::chrono::milliseconds delay = noStepDelay )
    {
        grab::config::ScriptStep step;
        step.action   = action;
        step.delay_ms = static_cast<std::uint32_t>( delay.count() );
        return step;
    }

    [[nodiscard]]
    grab::config::ScriptSection
    make_script( bool                                            loop,
                 std::initializer_list<grab::config::ScriptStep> steps )
    {
        grab::config::ScriptSection script;
        script.loop = loop;
        script.steps.assign( steps );
        return script;
    }

    void
    finish_initial_capture( grab::config::WatchSchedule& schedule )
    {
        const grab::config::Due due = schedule.next( origin );
        EXPECT_EQ( due.kind, grab::config::DueKind::Capture );
        EXPECT_EQ( due.step_index, noStepIndex );
        EXPECT_EQ( due.wake_at, origin );
        schedule.capture_done( origin );
    }

}    // namespace

TEST( ConfigSchedule,
      FirstCaptureDueImmediately )
{
    grab::config::WatchSchedule schedule{ captureInterval, noScript };

    const grab::config::Due     due = schedule.next( origin );

    EXPECT_EQ( due.kind, grab::config::DueKind::Capture );
    EXPECT_EQ( due.step_index, noStepIndex );
    EXPECT_EQ( due.wake_at, origin );
    EXPECT_EQ( schedule.skipped_captures(), noSkipped );
}

TEST( ConfigSchedule,
      FixedDelayNotFixedRate )
{
    grab::config::WatchSchedule schedule{ captureInterval, noScript };
    const auto                  captureFinished = origin + slowCaptureDuration;
    const auto                  expectedWake    = captureFinished + captureInterval;

    const grab::config::Due     first           = schedule.next( origin );
    ASSERT_EQ( first.kind, grab::config::DueKind::Capture );
    schedule.capture_done( captureFinished );
    const grab::config::Due next = schedule.next( captureFinished );

    EXPECT_EQ( next.kind, grab::config::DueKind::Capture );
    EXPECT_EQ( next.wake_at, expectedWake );
    EXPECT_EQ( schedule.skipped_captures(), noSkipped );
}

TEST( ConfigSchedule,
      CaptureWinsTie )
{
    const grab::config::ScriptSection script =
        make_script( singlePass,
                     {
                         make_step( grab::config::StepAction::Move ),
                         make_step( grab::config::StepAction::Delay, captureInterval ),
                     } );
    grab::config::WatchSchedule schedule{ captureInterval, &script };
    const auto                  tiedDeadline = origin + captureInterval;

    finish_initial_capture( schedule );
    const grab::config::Due firstStep = schedule.next( origin );
    ASSERT_EQ( firstStep.kind, grab::config::DueKind::Step );
    ASSERT_EQ( firstStep.step_index, firstStepIndex );
    schedule.step_done( origin );
    const grab::config::Due tied = schedule.next( tiedDeadline );

    EXPECT_EQ( tied.kind, grab::config::DueKind::Capture );
    EXPECT_EQ( tied.wake_at, tiedDeadline );
    EXPECT_EQ( schedule.skipped_captures(), noSkipped );
}

TEST( ConfigSchedule,
      StepsInterleave )
{
    const grab::config::ScriptSection script =
        make_script( singlePass,
                     {
                         make_step( grab::config::StepAction::Move ),
                         make_step( grab::config::StepAction::Delay, interleavedDelay ),
                         make_step( grab::config::StepAction::Click ),
                     } );
    grab::config::WatchSchedule schedule{ captureInterval, &script };
    const auto                  captureDeadline = origin + captureInterval;
    const auto                  delayDeadline   = origin + interleavedDelay;

    finish_initial_capture( schedule );
    const grab::config::Due firstStep = schedule.next( origin );
    ASSERT_EQ( firstStep.kind, grab::config::DueKind::Step );
    EXPECT_EQ( firstStep.step_index, firstStepIndex );
    schedule.step_done( origin );

    const grab::config::Due capture = schedule.next( origin );
    ASSERT_EQ( capture.kind, grab::config::DueKind::Capture );
    EXPECT_EQ( capture.wake_at, captureDeadline );
    schedule.capture_done( captureDeadline );

    const grab::config::Due delay = schedule.next( captureDeadline );
    ASSERT_EQ( delay.kind, grab::config::DueKind::Step );
    EXPECT_EQ( delay.step_index, secondStepIndex );
    EXPECT_EQ( delay.wake_at, delayDeadline );
    schedule.step_done( delayDeadline );

    const grab::config::Due finalStep = schedule.next( delayDeadline );
    EXPECT_EQ( finalStep.kind, grab::config::DueKind::Step );
    EXPECT_EQ( finalStep.step_index, thirdStepIndex );
    EXPECT_EQ( finalStep.wake_at, delayDeadline );
}

TEST( ConfigSchedule,
      OverrunSkipsAndCounts )
{
    const grab::config::ScriptSection script =
        make_script( singlePass, { make_step( grab::config::StepAction::Type ) } );
    grab::config::WatchSchedule schedule{ captureInterval, &script };
    const auto                  stepFinished = origin + overrunDuration;
    const auto retainedDeadline = origin + captureInterval + captureInterval;
    const auto nextDeadline     = stepFinished + captureInterval;

    finish_initial_capture( schedule );
    const grab::config::Due step = schedule.next( origin );
    ASSERT_EQ( step.kind, grab::config::DueKind::Step );
    schedule.step_done( stepFinished );
    const grab::config::Due overdue = schedule.next( stepFinished );

    ASSERT_EQ( overdue.kind, grab::config::DueKind::Capture );
    EXPECT_EQ( overdue.wake_at, retainedDeadline );
    EXPECT_EQ( schedule.skipped_captures(), oneSkipped );

    const grab::config::Due repeated = schedule.next( stepFinished );
    EXPECT_EQ( repeated.kind, grab::config::DueKind::Capture );
    EXPECT_EQ( repeated.wake_at, retainedDeadline );
    EXPECT_EQ( schedule.skipped_captures(), oneSkipped );

    schedule.capture_done( stepFinished );
    const grab::config::Due afterCapture = schedule.next( stepFinished );
    EXPECT_EQ( afterCapture.kind, grab::config::DueKind::Capture );
    EXPECT_EQ( afterCapture.wake_at, nextDeadline );
    EXPECT_EQ( schedule.skipped_captures(), oneSkipped );
}

TEST( ConfigSchedule,
      LoopRestartsSteps )
{
    const grab::config::ScriptSection script =
        make_script( looping,
                     {
                         make_step( grab::config::StepAction::Move ),
                         make_step( grab::config::StepAction::Click ),
                     } );
    grab::config::WatchSchedule schedule{ captureInterval, &script };

    finish_initial_capture( schedule );
    const grab::config::Due firstStep = schedule.next( origin );
    ASSERT_EQ( firstStep.kind, grab::config::DueKind::Step );
    EXPECT_EQ( firstStep.step_index, firstStepIndex );
    schedule.step_done( origin );

    const grab::config::Due secondStep = schedule.next( origin );
    ASSERT_EQ( secondStep.kind, grab::config::DueKind::Step );
    EXPECT_EQ( secondStep.step_index, secondStepIndex );
    schedule.step_done( origin );

    const grab::config::Due restarted = schedule.next( origin );
    EXPECT_EQ( restarted.kind, grab::config::DueKind::Step );
    EXPECT_EQ( restarted.step_index, firstStepIndex );
    EXPECT_EQ( restarted.wake_at, origin );
}

TEST( ConfigSchedule,
      NoLoopFinishesSteps )
{
    const grab::config::ScriptSection script =
        make_script( singlePass,
                     {
                         make_step( grab::config::StepAction::Move ),
                         make_step( grab::config::StepAction::Click ),
                     } );
    grab::config::WatchSchedule schedule{ captureInterval, &script };
    const auto                  captureDeadline = origin + captureInterval;

    finish_initial_capture( schedule );
    const grab::config::Due firstStep = schedule.next( origin );
    ASSERT_EQ( firstStep.kind, grab::config::DueKind::Step );
    schedule.step_done( origin );
    const grab::config::Due secondStep = schedule.next( origin );
    ASSERT_EQ( secondStep.kind, grab::config::DueKind::Step );
    schedule.step_done( origin );

    const grab::config::Due onlyCapture = schedule.next( origin );
    EXPECT_EQ( onlyCapture.kind, grab::config::DueKind::Capture );
    EXPECT_EQ( onlyCapture.wake_at, captureDeadline );
}

TEST( ConfigSchedule,
      FailScriptStopsSteps )
{
    const grab::config::ScriptSection script =
        make_script( looping, { make_step( grab::config::StepAction::Move ) } );
    grab::config::WatchSchedule schedule{ captureInterval, &script };
    const auto                  firstDeadline  = origin + captureInterval;
    const auto                  secondDeadline = firstDeadline + captureInterval;

    finish_initial_capture( schedule );
    const grab::config::Due step = schedule.next( origin );
    ASSERT_EQ( step.kind, grab::config::DueKind::Step );
    schedule.fail_script();

    const grab::config::Due firstCapture = schedule.next( origin );
    ASSERT_EQ( firstCapture.kind, grab::config::DueKind::Capture );
    EXPECT_EQ( firstCapture.wake_at, firstDeadline );
    schedule.capture_done( firstDeadline );

    const grab::config::Due secondCapture = schedule.next( firstDeadline );
    EXPECT_EQ( secondCapture.kind, grab::config::DueKind::Capture );
    EXPECT_EQ( secondCapture.wake_at, secondDeadline );
}
