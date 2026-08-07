// The Player exercised with FABRICATED TIME and NO X SERVER.
//
// pump( now ) reads no clock of its own, so every assertion here is about
// virtual time: a five-second wait costs microseconds of real time, and a
// scheduler bug shows up as an arithmetic mismatch rather than as an
// intermittent.
//
// The command bodies come from a fake CommandRunner rather than from
// execute.hpp. That is the whole point of the seam: the frontier, the pacing
// and the unwind are testable without an X connection, a seat, or the command
// layer existing at all.

#include "grab/command.hpp"
#include "grab/command_descriptor.hpp"
#include "grab/drag.hpp"
#include "grab/geometry/point.hpp"
#include "grab/result.hpp"
#include "grab/sequence_types.hpp"
#include "grab/trace.hpp"
#include "kernel/sequence/player.hpp"
#include "kernel/sequence/sequence.hpp"
#include "kernel/support/log.hpp"
#include "kernel/support/step_diag.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
// clang-format on

namespace
{

    using Clock     = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Nanos     = std::chrono::nanoseconds;
    using Millis    = std::chrono::milliseconds;

    using grab::kernel::sequence::CommandRunner;
    using grab::kernel::sequence::Player;
    using grab::kernel::sequence::Sequence;
    using grab::sequence::PacingMode;
    using grab::sequence::PacingOptions;
    using grab::sequence::PlayState;
    using grab::sequence::Status;
    using grab::sequence::Step;
    using grab::sequence::StepId;
    using grab::sequence::StepStatus;

    // ---- named values, per CLAUDE.md §6: no magic numbers anywhere ----------

    constexpr TimePoint        origin{ Clock::duration::zero() };

    constexpr Millis           dragDwell{ 8 };
    constexpr std::size_t      dragWaypoints = 16U;
    constexpr Millis           dragSpan{ 128 };    // dragWaypoints * dragDwell

    constexpr Millis           longWait{ 5'000 };
    constexpr Millis           captureSpan{ 80 };
    constexpr Millis           declaredZero{ 0 };
    constexpr Millis           shortSpan{ 10 };
    constexpr Millis           mediumSpan{ 20 };
    constexpr Millis           graceSpan{ 50 };
    constexpr Millis           extraGraceSpan{ 30 };
    constexpr Millis           longGrace{ 200 };
    constexpr Millis           noGrace{ 0 };
    constexpr Millis           oneTick{ 1 };

    constexpr std::size_t      zeroEvents    = 0U;
    constexpr std::size_t      oneEvent      = 1U;
    constexpr std::size_t      twoEvents     = 2U;
    constexpr std::size_t      firstAttempt  = 1U;
    constexpr std::size_t      secondAttempt = 2U;
    constexpr std::size_t      maxPumps      = 4'096U;
    constexpr std::size_t      fewPumps      = 8U;
    constexpr std::size_t      noWaypoints   = 0U;

    constexpr std::int32_t     dragFromX     = 100;
    constexpr std::int32_t     dragFromY     = 200;
    constexpr std::int32_t     dragToX       = 300;
    constexpr std::int32_t     dragToY       = 200;

    constexpr std::string_view sequenceName  = "test";
    constexpr std::string_view targetLabel   = "target";
    constexpr std::string_view recoverLabel  = "recover";
    constexpr std::string_view absentLabel   = "no-such-label";

    // ---- the fake runner ----------------------------------------------------

    // Everything the fake did, in order. The ORDER is the assertion in several
    // tests: a join must precede the release it protects.
    struct RunnerEvent
    {
            enum class Kind : std::uint8_t
            {
                Enter,
                Waypoint,
                Press,
                Release,
                Join,
                Exit,
                // release_holds(): the seam that reaches back into a step the
                // unwind has already exited.
                Reap,
                Count,
            };

            Kind        kind{ Kind::Enter };
            StepId      step{};
            TimePoint   at{};
            std::size_t ordinal{ 0U };
    };

    // A scripted body. Every field has a default initializer, because
    // -Wmissing-designated-field-initializers is live and a skipped field
    // without one is a build failure at every call site.
    struct Script
    {
            // How long the body takes in virtual time. Zero plus no waypoints
            // is an Instant command: enter() answers Success and it never
            // needs a tick.
            Nanos                pace{ Nanos::zero() };
            std::optional<Nanos> declared{};
            std::size_t          waypoints{ 0U };
            Nanos                dwell{ Nanos::zero() };
            bool                 holds_button{ false };
            // An IMPLICIT hold: the body takes it and the body gives it back,
            // so exit() finds nothing left to do on the success path.
            //
            // An EXPLICIT one is different in kind. input.press, input.key_down
            // and overlay.grab leave something down ON PURPOSE for a LATER step
            // to lift, so the body must NOT give it back and exit() must not
            // either -- releasing it would make a chord unspellable. Only
            // release_holds() lifts it, and only when the run is being unwound.
            bool                 document_hold{ false };
            bool                 fails{ false };
            // Which attempt succeeds; 0 means every attempt fails.
            std::size_t          succeeds_on_attempt{ 0U };
            grab::ErrorCode      error{ grab::ErrorCode::ProviderFailed };
    };

    struct BodyState
    {
            TimePoint   started{};
            std::size_t emitted{ 0U };
            std::size_t attempts{ 0U };
            bool        pressed{ false };
            bool        released{ false };
            bool        document_held{ false };
    };

    [[nodiscard]]
    Nanos
    scaled( Nanos       unit,
            std::size_t count )
    {
        return unit * static_cast<Nanos::rep>( count );
    }

    class FakeRunner final : public CommandRunner
    {
        public:

            explicit FakeRunner( const Sequence& program ) :
                scripts_( program.steps().size() ),
                bodies_( program.steps().size() )
            {
                // time.wait is the one op whose duration the document states,
                // so the default body honours it. Everything else is Instant
                // until a test says otherwise.
                for( const auto& step : program.steps() )
                {
                    const auto* const wait =
                        std::get_if<grab::sequence::WaitCommand>( &step.command );
                    if( wait != nullptr )
                    {
                        scripts_[step.id.index()].pace     = wait->duration;
                        scripts_[step.id.index()].declared = wait->duration;
                    }
                }
            }

            [[nodiscard]]
            Script&
            script( StepId id )
            {
                return scripts_[id.index()];
            }

            [[nodiscard]]
            const std::vector<RunnerEvent>&
            log() const noexcept
            {
                return log_;
            }

            [[nodiscard]]
            std::size_t
            attempts( StepId id ) const
            {
                return bodies_[id.index()].attempts;
            }

            [[nodiscard]]
            Status
            enter( const Step& step,
                   TimePoint   now ) override
            {
                auto& body     = bodies_[step.id.index()];
                body.started   = now;
                body.emitted   = 0U;
                body.released  = false;
                body.attempts += 1U;
                last_now_      = now;
                log_.push_back( RunnerEvent{
                    .kind = RunnerEvent::Kind::Enter,
                    .step = step.id,
                    .at   = now
                } );
                if( scripts_[step.id.index()].holds_button )
                {
                    body.pressed = true;
                    log_.push_back( RunnerEvent{
                        .kind = RunnerEvent::Kind::Press,
                        .step = step.id,
                        .at   = now
                    } );
                }
                if( scripts_[step.id.index()].document_hold )
                {
                    body.document_held = true;
                    log_.push_back( RunnerEvent{
                        .kind = RunnerEvent::Kind::Press,
                        .step = step.id,
                        .at   = now
                    } );
                }
                return progress( step, now );
            }

            [[nodiscard]]
            Status
            tick( const Step& step,
                  TimePoint   now ) override
            {
                last_now_ = now;
                return progress( step, now );
            }

            grab::NeutralizationOutcome
            exit( const Step& step,
                  TimePoint   now ) override
            {
                last_now_ = now;
                log_.push_back( RunnerEvent{
                    .kind = RunnerEvent::Kind::Exit,
                    .step = step.id,
                    .at   = now
                } );
                auto& body = bodies_[step.id.index()];
                if( body.pressed && !body.released )
                {
                    body.pressed  = false;
                    body.released = true;
                    log_.push_back( RunnerEvent{
                        .kind = RunnerEvent::Kind::Release,
                        .step = step.id,
                        .at   = now
                    } );
                    return grab::NeutralizationOutcome::Released;
                }
                return grab::NeutralizationOutcome::NothingHeld;
            }

            void
            join( const Step& step ) override
            {
                log_.push_back( RunnerEvent{
                    .kind = RunnerEvent::Kind::Join,
                    .step = step.id,
                    .at   = last_now_
                } );
            }

            // The seam under test. It lifts ONLY the explicit hold, and it is
            // reached only for steps the unwind has already exited -- which is
            // every step that completed cleanly.
            grab::NeutralizationOutcome
            release_holds( const Step& step ) override
            {
                log_.push_back( RunnerEvent{
                    .kind = RunnerEvent::Kind::Reap,
                    .step = step.id,
                    .at   = last_now_
                } );
                auto& body = bodies_[step.id.index()];
                if( !body.document_held )
                {
                    return grab::NeutralizationOutcome::NotAttempted;
                }
                body.document_held = false;
                log_.push_back( RunnerEvent{
                    .kind = RunnerEvent::Kind::Release,
                    .step = step.id,
                    .at   = last_now_
                } );
                return grab::NeutralizationOutcome::Released;
            }

            [[nodiscard]]
            grab::ErrorCode
            last_error( const Step& step ) const override
            {
                return scripts_[step.id.index()].error;
            }

            [[nodiscard]]
            std::optional<TimePoint>
            next_tick( const Step& step ) const override
            {
                const auto&              script_ref = scripts_[step.id.index()];
                const auto&              body       = bodies_[step.id.index()];

                std::optional<TimePoint> best;
                if( body.emitted <
                    script_ref.waypoints &&
                    script_ref.dwell > Nanos::zero() )
                {
                    best = body.started + scaled( script_ref.dwell, body.emitted + 1U );
                }
                const auto completion = body.started + script_ref.pace;
                if( !best.has_value() || completion < *best )
                {
                    best = completion;
                }
                return best;
            }

            [[nodiscard]]
            std::optional<Nanos>
            declared_duration( const Step& step ) const override
            {
                return scripts_[step.id.index()].declared;
            }

        private:

            [[nodiscard]]
            Status
            progress( const Step& step,
                      TimePoint   now )
            {
                auto& script_ref = scripts_[step.id.index()];
                auto& body       = bodies_[step.id.index()];

                while( body.emitted <
                       script_ref.waypoints &&
                       script_ref.dwell >
                       Nanos::zero() &&
                       body.started +
                       scaled( script_ref.dwell, body.emitted + 1U ) <= now )
                {
                    body.emitted += 1U;
                    log_.push_back( RunnerEvent{
                        .kind    = RunnerEvent::Kind::Waypoint,
                        .step    = step.id,
                        .at      = now,
                        .ordinal = body.emitted
                    } );
                }

                if( now <
                    body.started +
                    script_ref.pace ||
                    body.emitted < script_ref.waypoints )
                {
                    return Status::Running;
                }
                if( script_ref.fails && body.attempts != script_ref.succeeds_on_attempt )
                {
                    return Status::Failure;
                }
                // A body that ran to completion releases what it pressed, the
                // way execute_drag does. exit() afterwards therefore reports
                // NothingHeld, and only an interrupted body reports Released.
                if( body.pressed && !body.released )
                {
                    body.pressed  = false;
                    body.released = true;
                    log_.push_back( RunnerEvent{
                        .kind = RunnerEvent::Kind::Release,
                        .step = step.id,
                        .at   = now
                    } );
                }
                return Status::Success;
            }

            std::vector<Script>      scripts_;
            std::vector<BodyState>   bodies_;
            std::vector<RunnerEvent> log_;
            TimePoint                last_now_{};
    };

    // ---- document helpers ---------------------------------------------------

    [[nodiscard]]
    StepId
    positional_id( std::size_t index )
    {
        return StepId{ static_cast<StepId::Half>( index ), StepId::firstGeneration };
    }

    [[nodiscard]]
    grab::sequence::Command
    a_click()
    {
        return grab::sequence::ClickCommand{};
    }

    [[nodiscard]]
    grab::sequence::Command
    a_drag()
    {
        return grab::sequence::DragCommand{
            .from    = grab::geometry::Point{ .x = dragFromX, .y = dragFromY },
            .to      = grab::geometry::Point{ .x = dragToX, .y = dragToY },
            .button  = grab::input::primaryButton,
            .options = grab::input::DragOptions{},
        };
    }

    [[nodiscard]]
    grab::sequence::Command
    a_capture()
    {
        return grab::sequence::CaptureCommand{ .output = "shot.png", .locator = {} };
    }

    [[nodiscard]]
    grab::sequence::Command
    a_wait( Nanos duration )
    {
        return grab::sequence::WaitCommand{ .duration = duration };
    }

    [[nodiscard]]
    Step
    make_step( grab::sequence::Command command,
               std::vector<StepId>     after,
               std::string             label = {} )
    {
        return Step{
            .id              = {},
            .label           = std::move( label ),
            .command         = std::move( command ),
            .after           = std::move( after ),
            .on_error        = grab::sequence::ErrorPolicy::Abort,
            .on_error_target = {},
            .extra_grace     = Millis::zero(),
        };
    }

    [[nodiscard]]
    Sequence
    build_or_die( std::vector<Step> steps,
                  PacingOptions     pacing = PacingOptions{} )
    {
        auto built =
            Sequence::build( std::move( steps ), pacing, std::string{ sequenceName } );
        EXPECT_TRUE( built.has_value() )
            << ( built.has_value() ? std::string{} : built.error().message );
        return std::move( *built );
    }

    // Drive the run off next_deadline(), which is exactly what a timer thread
    // would arm. Returns the number of pumps, so a test can assert that five
    // seconds of virtual time cost a handful of them.
    [[nodiscard]]
    std::size_t
    run_to_completion( Player&   player,
                       TimePoint start )
    {
        auto        now   = start;
        std::size_t pumps = 0U;
        while( pumps < maxPumps )
        {
            const auto pumped = player.pump( now );
            ++pumps;
            if( !pumped.has_value() )
            {
                break;
            }
            if( player.state() != PlayState::Playing )
            {
                break;
            }
            const auto deadline = player.next_deadline();
            if( !deadline.has_value() )
            {
                break;
            }
            now = std::max( *deadline, now + Nanos{ 1 } );
        }
        return pumps;
    }

    [[nodiscard]]
    std::vector<RunnerEvent>
    events_of_kind( const FakeRunner& runner,
                    RunnerEvent::Kind kind,
                    StepId            step )
    {
        std::vector<RunnerEvent> found;
        for( const auto& event : runner.log() )
        {
            if( event.kind == kind && event.step == step )
            {
                found.push_back( event );
            }
        }
        return found;
    }

    [[nodiscard]]
    std::optional<std::size_t>
    position_of( const FakeRunner& runner,
                 RunnerEvent::Kind kind,
                 StepId            step )
    {
        for( std::size_t index = 0U; index < runner.log().size(); ++index )
        {
            const auto& event = runner.log()[index];
            if( event.kind == kind && event.step == step )
            {
                return index;
            }
        }
        return std::nullopt;
    }

    // ---- waypoint cadence ---------------------------------------------------

    TEST( Player,
          DragEmitsWaypointsAtEightMillisecondIntervals )
    {
        const auto drag     = positional_id( 0U );
        auto       document = build_or_die( { make_step( a_drag(), {} ) } );
        FakeRunner runner{ document };
        runner.script( drag ).waypoints    = dragWaypoints;
        runner.script( drag ).dwell        = dragDwell;
        runner.script( drag ).pace         = dragSpan;
        runner.script( drag ).holds_button = true;

        Player player{ document, runner };
        ASSERT_TRUE( player.play().has_value() );
        ( void )run_to_completion( player, origin );

        EXPECT_EQ( player.state(), PlayState::Done );
        EXPECT_EQ( player.status_of( drag ), StepStatus::Succeeded );

        const auto waypoints =
            events_of_kind( runner, RunnerEvent::Kind::Waypoint, drag );
        ASSERT_EQ( waypoints.size(), dragWaypoints );
        for( std::size_t index = 0U; index < waypoints.size(); ++index )
        {
            EXPECT_EQ( waypoints[index].at - origin, scaled( dragDwell, index + 1U ) )
                << "waypoint " << index;
            EXPECT_EQ( waypoints[index].ordinal, index + 1U );
        }
    }

    // ---- virtual time -------------------------------------------------------

    TEST( Player,
          FiveSecondWaitBlocksItsSuccessorInVirtualTime )
    {
        const auto wait      = positional_id( 0U );
        const auto successor = positional_id( 1U );
        auto       document  = build_or_die( {
            make_step( a_wait( longWait ), {} ),
            make_step( a_click(), { wait } ),
        } );
        FakeRunner runner{ document };

        Player     player{ document, runner };
        ASSERT_TRUE( player.play().has_value() );
        const auto pumps = run_to_completion( player, origin );

        EXPECT_EQ( player.state(), PlayState::Done );
        ASSERT_TRUE( player.entered_at( successor ).has_value() );
        EXPECT_EQ( *player.entered_at( successor ) - origin, longWait );
        EXPECT_EQ( player.elapsed(), Nanos{ longWait } );
        // Deadline-driven, so five seconds of virtual time cost a handful of
        // pumps rather than five thousand.
        EXPECT_LE( pumps, fewPumps );
    }

    // ---- overrun ------------------------------------------------------------

    TEST( Player,
          OpaqueStepDeclaringZeroButTakingEightyDelaysSuccessorsAndReportsOverrun )
    {
        const auto capture   = positional_id( 0U );
        const auto successor = positional_id( 1U );
        auto       document  = build_or_die( {
            make_step( a_capture(), {} ),
            make_step( a_click(), { capture } ),
        } );
        FakeRunner runner{ document };
        runner.script( capture ).pace     = captureSpan;
        runner.script( capture ).declared = Nanos{ declaredZero };

        Player player{ document, runner };
        ASSERT_TRUE( player.play().has_value() );
        ( void )run_to_completion( player, origin );

        EXPECT_EQ( player.state(), PlayState::Done );
        ASSERT_TRUE( player.entered_at( successor ).has_value() );
        EXPECT_EQ( *player.entered_at( successor ) - origin, captureSpan );

        const auto timing = player.timing_of( capture );
        ASSERT_TRUE( timing.declared.has_value() );
        EXPECT_EQ( *timing.declared, Nanos{ declaredZero } );
        EXPECT_EQ( timing.call_duration, Nanos{ captureSpan } );
        // Two clocks, reported separately, never blended.
        EXPECT_FALSE( timing.server_observed.has_value() );
        EXPECT_EQ( player.overrun_of( capture ), Nanos{ captureSpan } );
        EXPECT_EQ( player.overrun_of( successor ), Nanos::zero() );
    }

    // ---- parallelism --------------------------------------------------------

    TEST( Player,
          CaptureAndDragInterleaveRatherThanSerialising )
    {
        const auto drag     = positional_id( 0U );
        const auto capture  = positional_id( 1U );
        auto       document = build_or_die( {
            make_step( a_drag(), {} ),
            make_step( a_capture(), {} ),
        } );
        FakeRunner runner{ document };
        runner.script( drag ).waypoints    = dragWaypoints;
        runner.script( drag ).dwell        = dragDwell;
        runner.script( drag ).pace         = dragSpan;
        runner.script( drag ).holds_button = true;
        runner.script( capture ).pace      = captureSpan;

        Player player{ document, runner };
        ASSERT_TRUE( player.play().has_value() );
        ASSERT_TRUE( player.pump( origin ).has_value() );

        // Both roots are in flight at once, which is what "frontier" means.
        EXPECT_EQ( player.status_of( drag ), StepStatus::Running );
        EXPECT_EQ( player.status_of( capture ), StepStatus::Running );
        EXPECT_EQ( player.frontier().size(), twoEvents );

        ( void )run_to_completion( player, origin );
        EXPECT_EQ( player.state(), PlayState::Done );

        // Waypoints land while the capture is still in flight: serialising
        // would have put every one of them after the capture's exit.
        const auto capture_exit =
            position_of( runner, RunnerEvent::Kind::Exit, capture );
        ASSERT_TRUE( capture_exit.has_value() );
        std::size_t before_capture_finished = 0U;
        for( std::size_t index = 0U; index < *capture_exit; ++index )
        {
            if( runner.log()[index].kind ==
                RunnerEvent::Kind::Waypoint &&
                runner.log()[index].step == drag )
            {
                ++before_capture_finished;
            }
        }
        EXPECT_GT( before_capture_finished, zeroEvents );
    }

    TEST( Player,
          BlockingStepDoesNotSlipDeadlinesInAParallelBranch )
    {
        // Control: the drag alone.
        const auto soloDrag = positional_id( 0U );
        auto       solo     = build_or_die( { make_step( a_drag(), {} ) } );
        FakeRunner soloRunner{ solo };
        soloRunner.script( soloDrag ).waypoints    = dragWaypoints;
        soloRunner.script( soloDrag ).dwell        = dragDwell;
        soloRunner.script( soloDrag ).pace         = dragSpan;
        soloRunner.script( soloDrag ).holds_button = true;
        Player soloPlayer{ solo, soloRunner };
        ASSERT_TRUE( soloPlayer.play().has_value() );
        ( void )run_to_completion( soloPlayer, origin );

        // The same drag beside an 80 ms blocking capture.
        const auto pairedDrag    = positional_id( 0U );
        const auto pairedCapture = positional_id( 1U );
        auto       paired        = build_or_die( {
            make_step( a_drag(), {} ),
            make_step( a_capture(), {} ),
        } );
        FakeRunner pairedRunner{ paired };
        pairedRunner.script( pairedDrag ).waypoints    = dragWaypoints;
        pairedRunner.script( pairedDrag ).dwell        = dragDwell;
        pairedRunner.script( pairedDrag ).pace         = dragSpan;
        pairedRunner.script( pairedDrag ).holds_button = true;
        pairedRunner.script( pairedCapture ).pace      = captureSpan;
        Player pairedPlayer{ paired, pairedRunner };
        ASSERT_TRUE( pairedPlayer.play().has_value() );
        ( void )run_to_completion( pairedPlayer, origin );

        const auto control =
            events_of_kind( soloRunner, RunnerEvent::Kind::Waypoint, soloDrag );
        const auto beside =
            events_of_kind( pairedRunner, RunnerEvent::Kind::Waypoint, pairedDrag );
        ASSERT_EQ( control.size(), dragWaypoints );
        ASSERT_EQ( beside.size(), dragWaypoints );
        for( std::size_t index = 0U; index < control.size(); ++index )
        {
            EXPECT_EQ( beside[index].at, control[index].at )
                << "waypoint " << index << " slipped beside a blocking step";
        }
    }

    // ---- interrupt ----------------------------------------------------------

    TEST( Player,
          InterruptMidDragEmitsTheButtonRelease )
    {
        const auto drag     = positional_id( 0U );
        auto       document = build_or_die( { make_step( a_drag(), {} ) } );
        FakeRunner runner{ document };
        runner.script( drag ).waypoints    = dragWaypoints;
        runner.script( drag ).dwell        = dragDwell;
        runner.script( drag ).pace         = dragSpan;
        runner.script( drag ).holds_button = true;

        Player player{ document, runner };
        ASSERT_TRUE( player.play().has_value() );
        ASSERT_TRUE( player.pump( origin ).has_value() );
        ASSERT_TRUE( player.pump( origin + dragDwell ).has_value() );
        ASSERT_EQ( player.status_of( drag ), StepStatus::Running );

        ASSERT_TRUE( player.interrupt().has_value() );

        EXPECT_EQ( player.state(), PlayState::Interrupted );
        EXPECT_EQ( player.status_of( drag ), StepStatus::Skipped );
        EXPECT_EQ( player.neutralization(), grab::NeutralizationOutcome::Released );

        const auto releases = events_of_kind( runner, RunnerEvent::Kind::Release, drag );
        ASSERT_EQ( releases.size(), oneEvent );
        EXPECT_EQ( releases.front().at, origin + dragDwell );
        // A partial drag: the button came back up long before the walk ended.
        const auto waypoints =
            events_of_kind( runner, RunnerEvent::Kind::Waypoint, drag );
        EXPECT_LT( waypoints.size(), dragWaypoints );
    }

    TEST( Player,
          InterruptDuringABlockingBodyJoinsBeforeTheRelease )
    {
        // Entry order is document order for roots, so the unwind — reverse
        // entry order — reaches the capture first and the drag second.
        const auto drag     = positional_id( 0U );
        const auto capture  = positional_id( 1U );
        auto       document = build_or_die( {
            make_step( a_drag(), {} ),
            make_step( a_capture(), {} ),
        } );
        FakeRunner runner{ document };
        runner.script( drag ).waypoints    = dragWaypoints;
        runner.script( drag ).dwell        = dragDwell;
        runner.script( drag ).pace         = dragSpan;
        runner.script( drag ).holds_button = true;
        runner.script( capture ).pace      = captureSpan;

        Player player{ document, runner };
        ASSERT_TRUE( player.play().has_value() );
        ASSERT_TRUE( player.pump( origin ).has_value() );
        ASSERT_TRUE( player.pump( origin + dragDwell ).has_value() );
        ASSERT_EQ( player.status_of( capture ), StepStatus::Running );

        ASSERT_TRUE( player.interrupt().has_value() );

        const auto join = position_of( runner, RunnerEvent::Kind::Join, capture );
        const auto capture_exit =
            position_of( runner, RunnerEvent::Kind::Exit, capture );
        const auto release = position_of( runner, RunnerEvent::Kind::Release, drag );

        // The worker is joined before its own exit() runs, and the whole
        // unwind — the release included — happens after it. Nothing races a
        // capture still writing its buffer.
        ASSERT_TRUE( join.has_value() );
        ASSERT_TRUE( capture_exit.has_value() );
        ASSERT_TRUE( release.has_value() );
        EXPECT_LT( *join, *capture_exit );
        EXPECT_LT( *join, *release );

        // Only the Blocking command is joined; the drag is not blocking.
        EXPECT_EQ( events_of_kind( runner, RunnerEvent::Kind::Join, drag ).size(),
                   zeroEvents );
        EXPECT_EQ( events_of_kind( runner, RunnerEvent::Kind::Join, capture ).size(),
                   oneEvent );
    }

    TEST( Player,
          InterruptExitsEnteredStepsInReverseEntryOrder )
    {
        const auto first    = positional_id( 0U );
        const auto second   = positional_id( 1U );
        auto       document = build_or_die( {
            make_step( a_drag(), {} ),
            make_step( a_drag(), {} ),
        } );
        FakeRunner runner{ document };
        for( const auto id : { first, second } )
        {
            runner.script( id ).waypoints    = dragWaypoints;
            runner.script( id ).dwell        = dragDwell;
            runner.script( id ).pace         = dragSpan;
            runner.script( id ).holds_button = true;
        }

        Player player{ document, runner };
        ASSERT_TRUE( player.play().has_value() );
        ASSERT_TRUE( player.pump( origin ).has_value() );
        ASSERT_EQ( player.entry_order().size(), twoEvents );
        EXPECT_EQ( player.entry_order()[0], first );
        EXPECT_EQ( player.entry_order()[1], second );

        ASSERT_TRUE( player.interrupt().has_value() );

        const auto firstExit  = position_of( runner, RunnerEvent::Kind::Exit, first );
        const auto secondExit = position_of( runner, RunnerEvent::Kind::Exit, second );
        ASSERT_TRUE( firstExit.has_value() );
        ASSERT_TRUE( secondExit.has_value() );
        EXPECT_LT( *secondExit, *firstExit );
    }

    // ---- skip ---------------------------------------------------------------

    TEST( Player,
          SkipOnARunningStepRunsItsExit )
    {
        const auto drag      = positional_id( 0U );
        const auto successor = positional_id( 1U );
        auto       document  = build_or_die( {
            make_step( a_drag(), {} ),
            make_step( a_click(), { drag } ),
        } );
        FakeRunner runner{ document };
        runner.script( drag ).waypoints    = dragWaypoints;
        runner.script( drag ).dwell        = dragDwell;
        runner.script( drag ).pace         = dragSpan;
        runner.script( drag ).holds_button = true;

        Player player{ document, runner };
        ASSERT_TRUE( player.play().has_value() );
        ASSERT_TRUE( player.pump( origin ).has_value() );
        ASSERT_EQ( player.status_of( drag ), StepStatus::Running );

        ASSERT_TRUE( player.skip().has_value() );

        // Skipping never strands a held button.
        EXPECT_EQ( events_of_kind( runner, RunnerEvent::Kind::Release, drag ).size(),
                   oneEvent );
        EXPECT_EQ( player.status_of( drag ), StepStatus::Skipped );
        EXPECT_EQ( player.neutralization(), grab::NeutralizationOutcome::Released );
        // ... and it advances to the successors.
        EXPECT_EQ( player.status_of( successor ), StepStatus::Ready );

        ASSERT_TRUE( player.pump( origin ).has_value() );
        EXPECT_EQ( player.status_of( successor ), StepStatus::Succeeded );
        EXPECT_EQ( player.state(), PlayState::Done );
    }

    // ---- pause --------------------------------------------------------------

    TEST( Player,
          PauseAdmitsNoNewStepsWhileRunningOnesFinish )
    {
        const auto running   = positional_id( 0U );
        const auto successor = positional_id( 1U );
        auto       document  = build_or_die( {
            make_step( a_wait( Nanos{ mediumSpan } ), {} ),
            make_step( a_click(), { running } ),
        } );
        FakeRunner runner{ document };

        Player     player{ document, runner };
        ASSERT_TRUE( player.play().has_value() );
        ASSERT_TRUE( player.pump( origin ).has_value() );
        ASSERT_EQ( player.status_of( running ), StepStatus::Running );

        ASSERT_TRUE( player.pause().has_value() );
        EXPECT_EQ( player.state(), PlayState::Paused );

        ASSERT_TRUE( player.pump( origin + mediumSpan ).has_value() );
        // The running step ran to completion — pausing mid-drag would strand a
        // held button — but nothing new was admitted.
        EXPECT_EQ( player.status_of( running ), StepStatus::Succeeded );
        EXPECT_EQ( player.status_of( successor ), StepStatus::Ready );
        EXPECT_EQ( player.state(), PlayState::Paused );

        ASSERT_TRUE( player.play().has_value() );
        ASSERT_TRUE( player.pump( origin + mediumSpan ).has_value() );
        EXPECT_EQ( player.status_of( successor ), StepStatus::Succeeded );
        EXPECT_EQ( player.state(), PlayState::Done );
    }

    // ---- pacing -------------------------------------------------------------

    [[nodiscard]]
    std::vector<Step>
    three_step_chain()
    {
        const auto first  = positional_id( 0U );
        const auto second = positional_id( 1U );
        auto       steps  = std::vector<Step>{
            make_step( a_wait( Nanos{ shortSpan } ), {} ),
            make_step( a_wait( Nanos{ shortSpan } ), { first } ),
            make_step( a_wait( Nanos{ shortSpan } ), { second } ),
        };
        steps[1].extra_grace = extraGraceSpan;
        return steps;
    }

    TEST( Player,
          PacingModesShareStepIdsAndDifferInTotals )
    {
        const auto strict = build_or_die(
            three_step_chain(),
            PacingOptions{ .mode = PacingMode::Strict, .grace = graceSpan }
        );
        const auto grace = build_or_die(
            three_step_chain(),
            PacingOptions{ .mode = PacingMode::Grace, .grace = graceSpan }
        );
        const auto precise = build_or_die(
            three_step_chain(),
            PacingOptions{ .mode = PacingMode::Precise, .grace = graceSpan }
        );

        // Grace is a SCHEDULING property, never injected nodes: injected waits
        // would change the step count and give the same document different
        // StepIds per mode.
        ASSERT_EQ( strict.steps().size(), grace.steps().size() );
        ASSERT_EQ( strict.steps().size(), precise.steps().size() );
        for( std::size_t index = 0U; index < strict.steps().size(); ++index )
        {
            EXPECT_EQ( strict.steps()[index].id, grace.steps()[index].id );
            EXPECT_EQ( strict.steps()[index].id, precise.steps()[index].id );
        }

        FakeRunner strictRunner{ strict };
        FakeRunner graceRunner{ grace };
        FakeRunner preciseRunner{ precise };
        Player     strictPlayer{ strict, strictRunner };
        Player     gracePlayer{ grace, graceRunner };
        Player     precisePlayer{ precise, preciseRunner };
        ASSERT_TRUE( strictPlayer.play().has_value() );
        ASSERT_TRUE( gracePlayer.play().has_value() );
        ASSERT_TRUE( precisePlayer.play().has_value() );
        ( void )run_to_completion( strictPlayer, origin );
        ( void )run_to_completion( gracePlayer, origin );
        ( void )run_to_completion( precisePlayer, origin );

        constexpr auto steps    = 3U;
        constexpr auto nonRoots = 2U;
        const auto     body     = scaled( Nanos{ shortSpan }, steps );
        EXPECT_EQ( strictPlayer.elapsed(), body );
        EXPECT_EQ( gracePlayer.elapsed(),
                   body + scaled( Nanos{ graceSpan }, nonRoots ) );
        EXPECT_EQ(
            precisePlayer.elapsed(),
            body + scaled( Nanos{ graceSpan }, nonRoots ) + Nanos{ extraGraceSpan }
        );
        EXPECT_LT( strictPlayer.elapsed(), gracePlayer.elapsed() );
        EXPECT_LT( gracePlayer.elapsed(), precisePlayer.elapsed() );
    }

    TEST( Player,
          StrictInsertsNothingAndGraceDelaysEveryNonRootStep )
    {
        const auto first  = positional_id( 0U );
        const auto second = positional_id( 1U );
        const auto third  = positional_id( 2U );

        const auto strict = build_or_die(
            three_step_chain(),
            PacingOptions{ .mode = PacingMode::Strict, .grace = graceSpan }
        );
        FakeRunner strictRunner{ strict };
        Player     strictPlayer{ strict, strictRunner };
        ASSERT_TRUE( strictPlayer.play().has_value() );
        ( void )run_to_completion( strictPlayer, origin );

        EXPECT_EQ( *strictPlayer.entered_at( first ), origin );
        EXPECT_EQ( *strictPlayer.entered_at( second ),
                   *strictPlayer.finished_at( first ) );
        EXPECT_EQ( *strictPlayer.entered_at( third ),
                   *strictPlayer.finished_at( second ) );

        const auto grace = build_or_die(
            three_step_chain(),
            PacingOptions{ .mode = PacingMode::Grace, .grace = graceSpan }
        );
        FakeRunner graceRunner{ grace };
        Player     gracePlayer{ grace, graceRunner };
        ASSERT_TRUE( gracePlayer.play().has_value() );
        ( void )run_to_completion( gracePlayer, origin );

        // Roots start immediately; every other step waits out grace_ms, and
        // step 1's extra_grace_ms is IGNORED outside precise.
        EXPECT_EQ( *gracePlayer.entered_at( first ), origin );
        EXPECT_EQ( *gracePlayer.entered_at( second ),
                   *gracePlayer.finished_at( first ) + graceSpan );
        EXPECT_EQ( *gracePlayer.entered_at( third ),
                   *gracePlayer.finished_at( second ) + graceSpan );
    }

    TEST( Player,
          PreciseAddsExtraGraceOnlyWhereDeclared )
    {
        const auto first   = positional_id( 0U );
        const auto second  = positional_id( 1U );
        const auto third   = positional_id( 2U );
        const auto precise = build_or_die(
            three_step_chain(),
            PacingOptions{ .mode = PacingMode::Precise, .grace = graceSpan }
        );
        FakeRunner runner{ precise };
        Player     player{ precise, runner };
        ASSERT_TRUE( player.play().has_value() );
        ( void )run_to_completion( player, origin );

        EXPECT_EQ( *player.entered_at( first ), origin );
        EXPECT_EQ( *player.entered_at( second ),
                   *player.finished_at( first ) + graceSpan + extraGraceSpan );
        EXPECT_EQ( *player.entered_at( third ),
                   *player.finished_at( second ) + graceSpan );
    }

    TEST( Player,
          TheModeIsTheSoleAuthorityOnGrace )
    {
        const auto chain = three_step_chain();
        const auto root  = chain[0];
        const auto extra = chain[1];
        const auto plain = chain[2];

        using grab::kernel::sequence::grace_before;
        for( const auto mode :
             { PacingMode::Strict, PacingMode::Grace, PacingMode::Precise } )
        {
            const PacingOptions pacing{ .mode = mode, .grace = graceSpan };
            // Roots start immediately in every mode.
            EXPECT_EQ( grace_before( root, pacing ), Nanos::zero() );
        }

        const PacingOptions strict{ .mode = PacingMode::Strict, .grace = graceSpan };
        EXPECT_EQ( grace_before( extra, strict ), Nanos::zero() );
        EXPECT_EQ( grace_before( plain, strict ), Nanos::zero() );

        const PacingOptions grace{ .mode = PacingMode::Grace, .grace = graceSpan };
        EXPECT_EQ( grace_before( extra, grace ), Nanos{ graceSpan } );
        EXPECT_EQ( grace_before( plain, grace ), Nanos{ graceSpan } );

        const PacingOptions precise{ .mode = PacingMode::Precise, .grace = graceSpan };
        EXPECT_EQ( grace_before( extra, precise ),
                   Nanos{ graceSpan } + Nanos{ extraGraceSpan } );
        EXPECT_EQ( grace_before( plain, precise ), Nanos{ graceSpan } );
    }

    TEST( Player,
          InterruptUnderGraceReleasesImmediately )
    {
        const auto drag     = positional_id( 0U );
        const auto opener   = positional_id( 1U );
        const auto waiting  = positional_id( 2U );
        auto       document = build_or_die(
            {
                make_step( a_drag(), {} ),
                make_step( a_click(), {} ),
                make_step( a_click(), { positional_id( 1U ) } ),
            },
            PacingOptions{ .mode = PacingMode::Grace, .grace = longGrace }
        );
        FakeRunner runner{ document };
        runner.script( drag ).waypoints    = dragWaypoints;
        runner.script( drag ).dwell        = dragDwell;
        runner.script( drag ).pace         = dragSpan;
        runner.script( drag ).holds_button = true;

        Player player{ document, runner };
        ASSERT_TRUE( player.play().has_value() );
        ASSERT_TRUE( player.pump( origin ).has_value() );
        ASSERT_EQ( player.status_of( opener ), StepStatus::Succeeded );
        // The successor is sitting out the grace period.
        ASSERT_EQ( player.status_of( waiting ), StepStatus::Ready );
        ASSERT_EQ( *player.ready_at( waiting ), origin + longGrace );

        ASSERT_TRUE( player.pump( origin + dragDwell ).has_value() );
        ASSERT_TRUE( player.interrupt().has_value() );

        const auto releases = events_of_kind( runner, RunnerEvent::Kind::Release, drag );
        ASSERT_EQ( releases.size(), oneEvent );
        // Immediately, not after the grace period: a held button must never
        // wait one out.
        EXPECT_EQ( releases.front().at, origin + dragDwell );
        EXPECT_LT( releases.front().at, origin + longGrace );
        EXPECT_EQ( player.status_of( waiting ), StepStatus::Skipped );
    }

    // ---- goto ---------------------------------------------------------------

    // A → B → C → D  with an unrelated branch A → E → F. D is the goto target;
    // E and F are neither ancestors nor descendants of it.
    [[nodiscard]]
    std::vector<Step>
    forked_chain()
    {
        const auto stepA = positional_id( 0U );
        const auto stepB = positional_id( 1U );
        const auto stepC = positional_id( 2U );
        const auto stepE = positional_id( 4U );
        return {
            make_step( a_wait( Nanos{ shortSpan } ), {} ),
            make_step( a_wait( Nanos{ shortSpan } ), { stepA } ),
            make_step( a_wait( Nanos{ shortSpan } ), { stepB } ),
            make_step( a_wait( Nanos{ shortSpan } ),
                       { stepC },
                       std::string{ targetLabel } ),
            make_step( a_wait( Nanos{ shortSpan } ), { stepA } ),
            make_step( a_wait( Nanos{ shortSpan } ), { stepE } ),
        };
    }

    TEST( Player,
          GotoStepSkipsAncestorsAndLeavesUnrelatedBranchesPending )
    {
        const auto stepA    = positional_id( 0U );
        const auto stepB    = positional_id( 1U );
        const auto stepC    = positional_id( 2U );
        const auto stepD    = positional_id( 3U );
        const auto stepE    = positional_id( 4U );
        const auto stepF    = positional_id( 5U );
        auto       document = build_or_die( forked_chain() );
        FakeRunner runner{ document };

        Player     player{ document, runner };
        ASSERT_TRUE( player.play().has_value() );
        ASSERT_TRUE( player.pump( origin ).has_value() );
        ASSERT_TRUE( player.pump( origin + shortSpan ).has_value() );
        ASSERT_EQ( player.status_of( stepA ), StepStatus::Succeeded );
        ASSERT_EQ( player.status_of( stepB ), StepStatus::Running );
        ASSERT_EQ( player.status_of( stepE ), StepStatus::Running );

        ASSERT_TRUE( player.goto_step( stepD ).has_value() );

        // Ancestors that had not already run are Skipped — computed by reverse
        // traversal, not by taking a prefix of order().
        EXPECT_EQ( player.status_of( stepA ), StepStatus::Succeeded );
        EXPECT_EQ( player.status_of( stepB ), StepStatus::Skipped );
        EXPECT_EQ( player.status_of( stepC ), StepStatus::Skipped );
        // The unrelated branch is dropped from the frontier and never waited
        // on. E had been entered, so it was exited; F was never touched.
        EXPECT_EQ( player.status_of( stepF ), StepStatus::Pending );
        ASSERT_EQ( player.frontier().size(), oneEvent );
        EXPECT_EQ( player.frontier()[0], stepD );

        ASSERT_TRUE( player.pump( origin + shortSpan ).has_value() );
        ASSERT_TRUE( player.pump( origin + shortSpan + shortSpan ).has_value() );

        // Done when the FRONTIER empties, not when every step is terminal.
        EXPECT_EQ( player.status_of( stepD ), StepStatus::Succeeded );
        EXPECT_EQ( player.status_of( stepF ), StepStatus::Pending );
        EXPECT_EQ( player.state(), PlayState::Done );
    }

    TEST( Player,
          GotoStepBackwardsIsAnError )
    {
        const auto stepA    = positional_id( 0U );
        const auto stepC    = positional_id( 2U );
        auto       document = build_or_die( forked_chain() );
        FakeRunner runner{ document };

        Player     player{ document, runner };
        ASSERT_TRUE( player.play().has_value() );
        ASSERT_TRUE( player.pump( origin ).has_value() );
        ASSERT_TRUE( player.pump( origin + shortSpan ).has_value() );
        ASSERT_TRUE( player.pump( origin + shortSpan + shortSpan ).has_value() );
        ASSERT_EQ( player.status_of( stepC ), StepStatus::Running );

        // A is an ancestor of the running C, and it has already run: backward
        // motion means re-running effects.
        const auto jumped = player.goto_step( stepA );
        ASSERT_FALSE( jumped.has_value() );
        EXPECT_EQ( jumped.error().code, grab::ErrorCode::InvalidArgument );
        EXPECT_NE( jumped.error().message.find( "forward-only" ), std::string::npos );
        EXPECT_EQ( player.state(), PlayState::Playing );
    }

    TEST( Player,
          GotoStepToARunningFrontierMemberNarrowsTheFrontierToIt )
    {
        const auto stepB    = positional_id( 1U );
        const auto stepE    = positional_id( 4U );
        auto       document = build_or_die( forked_chain() );
        FakeRunner runner{ document };

        Player     player{ document, runner };
        ASSERT_TRUE( player.play().has_value() );
        ASSERT_TRUE( player.pump( origin ).has_value() );
        ASSERT_TRUE( player.pump( origin + shortSpan ).has_value() );
        ASSERT_EQ( player.status_of( stepB ), StepStatus::Running );
        ASSERT_EQ( player.status_of( stepE ), StepStatus::Running );

        // B is already Running, so it keeps running; the parallel branch is
        // dropped, its entered step exited so nothing is stranded.
        ASSERT_TRUE( player.goto_step( stepB ).has_value() );
        EXPECT_EQ( player.status_of( stepB ), StepStatus::Running );
        EXPECT_EQ( player.status_of( stepE ), StepStatus::Skipped );
        ASSERT_EQ( player.frontier().size(), oneEvent );
        EXPECT_EQ( player.frontier()[0], stepB );
        EXPECT_EQ( events_of_kind( runner, RunnerEvent::Kind::Exit, stepE ).size(),
                   oneEvent );
    }

    TEST( Player,
          GotoLabelWithAnUnknownLabelIsAnError )
    {
        auto       document = build_or_die( forked_chain() );
        FakeRunner runner{ document };
        Player     player{ document, runner };
        ASSERT_TRUE( player.play().has_value() );
        ASSERT_TRUE( player.pump( origin ).has_value() );

        const auto jumped = player.goto_label( absentLabel );
        ASSERT_FALSE( jumped.has_value() );
        EXPECT_EQ( jumped.error().code, grab::ErrorCode::NoMatch );
        EXPECT_NE( jumped.error().message.find( absentLabel ), std::string::npos );
        // Not a silent no-op: the run is untouched.
        EXPECT_EQ( player.state(), PlayState::Playing );
    }

    TEST( Player,
          GotoLabelResolvesAKnownLabel )
    {
        const auto stepD    = positional_id( 3U );
        auto       document = build_or_die( forked_chain() );
        FakeRunner runner{ document };
        Player     player{ document, runner };
        ASSERT_TRUE( player.play().has_value() );
        ASSERT_TRUE( player.pump( origin ).has_value() );

        ASSERT_TRUE( player.goto_label( targetLabel ).has_value() );
        ASSERT_EQ( player.frontier().size(), oneEvent );
        EXPECT_EQ( player.frontier()[0], stepD );
    }

    // ---- failure ------------------------------------------------------------

    TEST( Player,
          AbortUnwindsInReverseEntryOrderAndReportsTheFailure )
    {
        const auto holder   = positional_id( 0U );
        const auto failing  = positional_id( 1U );
        auto       document = build_or_die( {
            make_step( a_drag(), {} ),
            make_step( a_drag(), {} ),
        } );
        FakeRunner runner{ document };
        runner.script( holder ).waypoints     = dragWaypoints;
        runner.script( holder ).dwell         = dragDwell;
        runner.script( holder ).pace          = dragSpan;
        runner.script( holder ).holds_button  = true;
        runner.script( failing ).holds_button = true;
        runner.script( failing ).fails        = true;
        runner.script( failing ).error        = grab::ErrorCode::PossiblyCommitted;

        Player player{ document, runner };
        ASSERT_TRUE( player.play().has_value() );
        const auto pumped = player.pump( origin );

        ASSERT_FALSE( pumped.has_value() );
        EXPECT_EQ( pumped.error().code, grab::ErrorCode::PossiblyCommitted );
        EXPECT_EQ( player.state(), PlayState::Interrupted );
        EXPECT_EQ( player.status_of( failing ), StepStatus::Failed );
        ASSERT_NE( player.failure(), nullptr );
        EXPECT_EQ( player.failure()->code, grab::ErrorCode::PossiblyCommitted );

        // Abort and interrupt() share ONE unwind path, so the still-running
        // drag is released too.
        EXPECT_EQ( events_of_kind( runner, RunnerEvent::Kind::Release, holder ).size(),
                   oneEvent );
        EXPECT_EQ( player.neutralization(), grab::NeutralizationOutcome::Released );
        // The failing step is exited before the earlier one.
        const auto failingExit = position_of( runner, RunnerEvent::Kind::Exit, failing );
        const auto holderExit  = position_of( runner, RunnerEvent::Kind::Exit, holder );
        ASSERT_TRUE( failingExit.has_value() );
        ASSERT_TRUE( holderExit.has_value() );
        EXPECT_LT( *failingExit, *holderExit );

        // A later pump does not re-report the same failure.
        EXPECT_TRUE( player.pump( origin + oneTick ).has_value() );
    }

    TEST( Player,
          ContinueRunsSuccessorsAnyway )
    {
        const auto failing   = positional_id( 0U );
        const auto successor = positional_id( 1U );
        auto       steps     = std::vector<Step>{
            make_step( a_click(), {} ),
            make_step( a_click(), { failing } ),
        };
        steps[0].on_error   = grab::sequence::ErrorPolicy::Continue;
        auto       document = build_or_die( std::move( steps ) );
        FakeRunner runner{ document };
        runner.script( failing ).fails = true;

        Player player{ document, runner };
        ASSERT_TRUE( player.play().has_value() );
        ASSERT_TRUE( player.pump( origin ).has_value() );

        EXPECT_EQ( player.status_of( failing ), StepStatus::Failed );
        EXPECT_EQ( player.status_of( successor ), StepStatus::Succeeded );
        EXPECT_EQ( player.state(), PlayState::Done );
    }

    TEST( Player,
          GotoOnErrorJumpsToTheRecoveryStep )
    {
        const auto failing  = positional_id( 0U );
        const auto skipped  = positional_id( 1U );
        const auto recovery = positional_id( 2U );
        auto       steps    = std::vector<Step>{
            make_step( a_click(), {} ),
            make_step( a_click(), { failing } ),
            make_step( a_click(), {}, std::string{ recoverLabel } ),
        };
        steps[0].on_error        = grab::sequence::ErrorPolicy::Goto;
        steps[0].on_error_target = std::string{ recoverLabel };
        auto       document      = build_or_die( std::move( steps ) );
        FakeRunner runner{ document };
        runner.script( failing ).fails = true;

        Player player{ document, runner };
        ASSERT_TRUE( player.play().has_value() );
        ASSERT_TRUE( player.pump( origin ).has_value() );

        EXPECT_EQ( player.status_of( failing ), StepStatus::Failed );
        EXPECT_EQ( player.status_of( skipped ), StepStatus::Pending );
        EXPECT_EQ( player.status_of( recovery ), StepStatus::Succeeded );
        EXPECT_EQ( player.state(), PlayState::Done );
    }

    // ---- retry policy -------------------------------------------------------

    TEST( Player,
          AnIdempotentStepIsRetriedOnce )
    {
        const auto capture  = positional_id( 0U );
        auto       document = build_or_die( { make_step( a_capture(), {} ) } );
        FakeRunner runner{ document };
        runner.script( capture ).fails               = true;
        runner.script( capture ).succeeds_on_attempt = secondAttempt;

        Player player{ document, runner };
        ASSERT_TRUE( player.play().has_value() );
        ASSERT_TRUE( player.pump( origin ).has_value() );

        EXPECT_EQ( player.status_of( capture ), StepStatus::Succeeded );
        EXPECT_EQ( runner.attempts( capture ), secondAttempt );
        EXPECT_EQ( player.state(), PlayState::Done );
    }

    TEST( Player,
          PossiblyCommittedIsNeverRetried )
    {
        const auto capture  = positional_id( 0U );
        auto       document = build_or_die( { make_step( a_capture(), {} ) } );
        FakeRunner runner{ document };
        runner.script( capture ).fails               = true;
        runner.script( capture ).succeeds_on_attempt = secondAttempt;
        // screen.capture is Idempotent in the descriptor table, so only the
        // error code can stop the retry — and it must.
        runner.script( capture ).error = grab::ErrorCode::PossiblyCommitted;

        Player player{ document, runner };
        ASSERT_TRUE( player.play().has_value() );
        const auto pumped = player.pump( origin );

        ASSERT_FALSE( pumped.has_value() );
        EXPECT_EQ( player.status_of( capture ), StepStatus::Failed );
        EXPECT_EQ( runner.attempts( capture ), firstAttempt );
    }

    TEST( Player,
          RetryClassIsReadFromTheDescriptorTable )
    {
        using grab::kernel::sequence::may_retry;
        using grab::kernel::sequence::retry_class_of_step;

        const auto capture = make_step( a_capture(), {} );
        const auto drag    = make_step( a_drag(), {} );

        EXPECT_EQ( retry_class_of_step( capture ), grab::RetryClass::Idempotent );
        EXPECT_EQ( retry_class_of_step( drag ), grab::RetryClass::Never );

        EXPECT_TRUE( may_retry( capture, grab::ErrorCode::ProviderFailed ) );
        EXPECT_FALSE( may_retry( drag, grab::ErrorCode::ProviderFailed ) );
        // Never, regardless of class: a failed drag has a button down.
        EXPECT_FALSE( may_retry( capture, grab::ErrorCode::PossiblyCommitted ) );
        EXPECT_FALSE( may_retry( drag, grab::ErrorCode::PossiblyCommitted ) );
    }

    // ---- lifecycle odds and ends -------------------------------------------

    TEST( Player,
          AnEmptyDocumentReachesDoneOnTheFirstPump )
    {
        const Sequence document;
        FakeRunner     runner{ document };
        Player         player{ document, runner };

        EXPECT_EQ( player.state(), PlayState::Idle );
        ASSERT_TRUE( player.play().has_value() );
        ASSERT_TRUE( player.pump( origin ).has_value() );
        EXPECT_EQ( player.state(), PlayState::Done );
        EXPECT_TRUE( player.frontier().empty() );
        EXPECT_EQ( player.elapsed(), Nanos::zero() );
    }

    TEST( Player,
          RunIdIsMintedOncePerRunAndDiffersBetweenRuns )
    {
        auto         document = build_or_die( { make_step( a_click(), {} ) } );
        FakeRunner   first{ document };
        FakeRunner   second{ document };
        const Player one{ document, first };
        const Player two{ document, second };

        EXPECT_FALSE( one.run_id().is_nil() );
        EXPECT_FALSE( two.run_id().is_nil() );
        EXPECT_NE( one.run_id(), two.run_id() );
        EXPECT_EQ( one.program(), &document );
    }

    TEST( Player,
          ControlVerbsRejectAStateThatCannotHonourThem )
    {
        auto       document = build_or_die( { make_step( a_click(), {} ) } );
        FakeRunner runner{ document };
        Player     player{ document, runner };

        EXPECT_FALSE( player.pause().has_value() );
        EXPECT_FALSE( player.interrupt().has_value() );
        EXPECT_FALSE( player.skip().has_value() );
        EXPECT_FALSE( player.goto_step( positional_id( 0U ) ).has_value() );

        ASSERT_TRUE( player.play().has_value() );
        ASSERT_TRUE( player.pump( origin ).has_value() );
        ASSERT_EQ( player.state(), PlayState::Done );
        EXPECT_FALSE( player.play().has_value() );
        EXPECT_FALSE( player.next_deadline().has_value() );
    }

    TEST( Player,
          InstantStepsNeedNoTickAndReportNoDeclaredDuration )
    {
        const auto click    = positional_id( 0U );
        auto       document = build_or_die( { make_step( a_click(), {} ) } );
        FakeRunner runner{ document };

        Player     player{ document, runner };
        ASSERT_TRUE( player.play().has_value() );
        ASSERT_TRUE( player.pump( origin ).has_value() );

        EXPECT_EQ( player.status_of( click ), StepStatus::Succeeded );
        EXPECT_EQ( runner.attempts( click ), firstAttempt );
        EXPECT_EQ( events_of_kind( runner, RunnerEvent::Kind::Waypoint, click ).size(),
                   noWaypoints );
        // nullopt means UNKNOWN, SO MEASURE IT — never zero.
        EXPECT_FALSE( player.timing_of( click ).declared.has_value() );
        EXPECT_EQ( player.overrun_of( click ), Nanos::zero() );
    }

    // ---- the explicit hold, and the seam that lifts it ----------------------

    // THE DEFECT THIS CLOSES. succeed() exits a cleanly-completed step and
    // marks it exited; unwind() skips every step it has already exited. That is
    // right for an implicit hold, which exit() released, and WRONG for an
    // explicit one -- a sequence that presses, succeeds, and then aborts on a
    // later step used to leave the button physically down, silently, because no
    // code path could ever reach it again.
    TEST( Player,
          UnwindReleasesAHoldLeftByAStepThatALREADYSucceeded )
    {
        const auto held     = positional_id( 0U );
        const auto doomed   = positional_id( 1U );
        auto       document = build_or_die( {
            make_step( a_click(), {} ),
            make_step( a_click(), { held } ),
        } );

        FakeRunner runner{ document };
        runner.script( held ).document_hold         = true;
        runner.script( doomed ).fails               = true;
        runner.script( doomed ).succeeds_on_attempt = 0U;

        Player player{ document, runner };
        ASSERT_TRUE( player.play().has_value() );
        EXPECT_FALSE( player.pump( origin ).has_value() );

        EXPECT_EQ( player.status_of( held ), StepStatus::Succeeded );
        EXPECT_EQ( player.state(), PlayState::Interrupted );

        // exit() ran once, on the success path, and did NOT lift the hold --
        // lifting it there would make a chord unspellable.
        EXPECT_EQ( events_of_kind( runner, RunnerEvent::Kind::Exit, held ).size(),
                   oneEvent );
        // release_holds() reached it during the unwind, and lifted it exactly
        // once. This is a DISTINCT seam, not a second exit(): calling exit()
        // twice would double-release the implicit case.
        EXPECT_EQ( events_of_kind( runner, RunnerEvent::Kind::Reap, held ).size(),
                   oneEvent );
        EXPECT_EQ( events_of_kind( runner, RunnerEvent::Kind::Release, held ).size(),
                   oneEvent );
        EXPECT_EQ( player.neutralization(), grab::NeutralizationOutcome::Released );

        const auto exited = position_of( runner, RunnerEvent::Kind::Exit, held );
        const auto reaped = position_of( runner, RunnerEvent::Kind::Reap, held );
        ASSERT_TRUE( exited.has_value() );
        ASSERT_TRUE( reaped.has_value() );
        EXPECT_LT( *exited, *reaped );
    }

    // The other half of the contract: a run that ends cleanly is not unwound,
    // so nothing is reaped and the report still says nothing was neutralized.
    // Without this, a release_holds() called on every completion would look
    // just as green while releasing a chord's modifier mid-chord.
    TEST( Player,
          ACleanRunReapsNothing )
    {
        const auto first    = positional_id( 0U );
        const auto second   = positional_id( 1U );
        auto       document = build_or_die( {
            make_step( a_click(), {} ),
            make_step( a_click(), { first } ),
        } );

        FakeRunner runner{ document };
        runner.script( first ).document_hold = true;

        Player player{ document, runner };
        ASSERT_TRUE( player.play().has_value() );
        ASSERT_TRUE( player.pump( origin ).has_value() );

        EXPECT_EQ( player.state(), PlayState::Done );
        EXPECT_EQ( events_of_kind( runner, RunnerEvent::Kind::Reap, first ).size(),
                   zeroEvents );
        EXPECT_EQ( events_of_kind( runner, RunnerEvent::Kind::Reap, second ).size(),
                   zeroEvents );
        EXPECT_EQ( events_of_kind( runner, RunnerEvent::Kind::Release, first ).size(),
                   zeroEvents );
        EXPECT_EQ( player.neutralization(), grab::NeutralizationOutcome::NotAttempted );
    }

    // interrupt() is the same unwind, so the hold comes up there too -- and
    // that is the path a SIGINT mid-carry takes.
    TEST( Player,
          InterruptReapsAHoldLeftByAStepThatALREADYSucceeded )
    {
        const auto held     = positional_id( 0U );
        auto       document = build_or_die( {
            make_step( a_click(), {} ),
            make_step( a_wait( longWait ), { held } ),
        } );

        FakeRunner runner{ document };
        runner.script( held ).document_hold = true;

        Player player{ document, runner };
        ASSERT_TRUE( player.play().has_value() );
        ASSERT_TRUE( player.pump( origin ).has_value() );
        ASSERT_EQ( player.status_of( held ), StepStatus::Succeeded );
        EXPECT_EQ( events_of_kind( runner, RunnerEvent::Kind::Reap, held ).size(),
                   zeroEvents );

        ASSERT_TRUE( player.interrupt().has_value() );

        EXPECT_EQ( events_of_kind( runner, RunnerEvent::Kind::Release, held ).size(),
                   oneEvent );
        EXPECT_EQ( player.neutralization(), grab::NeutralizationOutcome::Released );
    }

    TEST( Player,
          PacingIsNeverInjectedAsNodes )
    {
        const auto strict = build_or_die(
            three_step_chain(),
            PacingOptions{ .mode = PacingMode::Strict, .grace = noGrace }
        );
        const auto grace = build_or_die(
            three_step_chain(),
            PacingOptions{ .mode = PacingMode::Grace, .grace = longGrace }
        );
        EXPECT_EQ( strict.steps().size(), grace.steps().size() );
        EXPECT_EQ( strict.order().size(), grace.order().size() );
    }

    // ---- introspection ------------------------------------------------------
    //
    // The subsystem's diagnosability is itself a contract: a DAG scheduler
    // whose frontier stalls is undebuggable without a record of the
    // transitions, and "where did the run spend its time" must be answerable
    // from a log rather than from a profiler.
    //
    // These assert on STRUCTURED VALUES -- an instrument tally, a timing_of(),
    // a `key=value` field -- and never on the prose around them (CLAUDE.md
    // §5). They also have to hold in a build whose log ceiling is `off`, where
    // the timers and the summary are both gone, so every one of them says what
    // it expects in each case rather than being compiled away.

    namespace phase = grab::kernel::sequence::phase;

    using Timer     = grab::diag::Measure<grab::log::Level::Debug>;
    using RunTimer  = grab::diag::Measure<grab::kernel::sequence::measureLevel>;

    constexpr std::string_view quickLabel    = "quick";
    constexpr std::string_view dominantLabel = "dominant";
    constexpr std::string_view trailingLabel = "trailing";
    constexpr std::string_view blockerLabel  = "blocker";
    constexpr std::string_view blockedLabel  = "blocked";
    constexpr std::string_view otherLabel    = "other";
    constexpr std::string_view gracedLabel   = "graced";
    constexpr std::string_view measuredName  = "test.measured";
    constexpr std::string_view runningText   = "running";
    constexpr std::string_view readyText     = "ready";
    constexpr std::string_view doneText      = "done";
    constexpr std::string_view preciseText   = "precise";
    constexpr std::string_view completeText  = "complete";

    constexpr Millis           dominantSpan{ 900 };
    constexpr Millis           tinySpan{ 1 };

    constexpr std::uint64_t    noCalls        = 0U;
    constexpr std::uint64_t    oneCall        = 1U;
    constexpr std::size_t      noTallies      = 0U;
    constexpr std::size_t      oneTally       = 1U;
    constexpr std::size_t      oneOccurrence  = 1U;
    constexpr std::size_t      soloFrontier   = 1U;
    constexpr std::size_t      pairedFrontier = 2U;
    constexpr std::int64_t     microsPerMilli = 1'000;

    // Routes the log to a temporary file for the duration of `body`, the way
    // tests/core/test_log.cpp does. The runtime level and the sink are
    // process-global, so both are restored.
    template<typename Body>
    [[nodiscard]]
    std::string
    captured_log( grab::log::Level level,
                  Body             body )
    {
        const std::string path =
            std::string{ std::tmpnam( nullptr ) } + ".grab-player-log";

        const auto previous = grab::log::runtime_level();
        EXPECT_TRUE( grab::log::sink_to_file( path ) );
        grab::log::set_runtime_level( level );

        body();

        grab::log::set_runtime_level( previous );
        grab::log::sink_off();

        std::ifstream     stream{ path };
        std::stringstream buffer;
        buffer << stream.rdbuf();
        stream.close();
        ( void )std::remove( path.c_str() );
        return buffer.str();
    }

    // A record is a sequence of ` key=value` fields. Matching the WHOLE field
    // is what keeps these assertions off the prose and stops a match landing
    // inside some other key's value.
    [[nodiscard]]
    std::string
    field( std::string_view key,
           std::string_view value )
    {
        std::string text{ " " };
        text.append( key );
        text.append( "=" );
        text.append( value );
        return text;
    }

    [[nodiscard]]
    std::string
    numeric_field( std::string_view key,
                   std::size_t      value )
    {
        const std::string text = std::to_string( value );
        return field( key, text );
    }

    [[nodiscard]]
    bool
    has_field( std::string_view text,
               std::string_view key,
               std::string_view value )
    {
        return text.find( field( key, value ) ) != std::string_view::npos;
    }

    [[nodiscard]]
    std::size_t
    occurrences( std::string_view text,
                 std::string_view needle )
    {
        std::size_t found = 0U;
        std::size_t at    = text.find( needle );
        while( at != std::string_view::npos )
        {
            ++found;
            at = text.find( needle, at + needle.size() );
        }
        return found;
    }

    [[nodiscard]]
    const grab::diag::Tally*
    tally_for( const grab::diag::Instrument& instrument,
               std::string_view              name )
    {
        for( const auto& entry : instrument.tallies() )
        {
            if( entry.name == name )
            {
                return &entry;
            }
        }
        return nullptr;
    }

    // sizeof is the only way to prove a member is ABSENT rather than merely
    // unread: a compiled-out timer that still carried a time_point would still
    // be reading the clock to fill it.
    TEST( Player,
          MeasureIsAnEmptyTimerWhenItsLevelIsCompiledOut )
    {
        // Where to record and under what name is unconditional -- that is the
        // handle. The CLOCK is what disappears.
        constexpr std::size_t handleSize =
            sizeof( grab::diag::Instrument* ) + sizeof( std::string_view );
        constexpr std::size_t liveSize = handleSize + sizeof( TimePoint );

        static_assert( Timer::enabled == grab::log::enabled( grab::log::Level::Debug ) );
        static_assert( sizeof( Timer ) == ( Timer::enabled ? liveSize : handleSize ) );
        static_assert( sizeof( grab::diag::Measure<grab::log::Level::Off> ) == liveSize,
                       "Off is always at or below the ceiling, so it is never "
                       "the compiled-out case" );

        grab::diag::Instrument instrument;
        {
            const Timer timer{ instrument, measuredName };
        }

        if constexpr( Timer::enabled )
        {
            ASSERT_EQ( instrument.tallies().size(), oneTally );
            EXPECT_EQ( instrument.tallies().front().name, measuredName );
            EXPECT_EQ( instrument.tallies().front().calls, oneCall );
        }
        else
        {
            // Untouched: no slot, no name, no clock read.
            EXPECT_EQ( instrument.tallies().size(), noTallies );
            EXPECT_EQ( instrument.total(), Nanos::zero() );
        }
        EXPECT_FALSE( instrument.overflowed() );
    }

    TEST( Player,
          InstrumentTalliesEveryCommandKindTheRunExecuted )
    {
        const auto click    = positional_id( 0U );
        const auto wait     = positional_id( 1U );
        const auto capture  = positional_id( 2U );
        auto       document = build_or_die( {
            make_step( a_click(), {} ),
            make_step( a_wait( Nanos{ shortSpan } ), { click } ),
            make_step( a_capture(), { wait } ),
        } );
        FakeRunner runner{ document };
        runner.script( capture ).pace = captureSpan;

        Player player{ document, runner };
        ASSERT_TRUE( player.play().has_value() );
        ( void )run_to_completion( player, origin );
        ASSERT_EQ( player.state(), PlayState::Done );

        const auto& instrument = player.instrument();
        // A dropped name would make every number below a partial accounting.
        EXPECT_FALSE( instrument.overflowed() );

        if constexpr( !RunTimer::enabled )
        {
            // The whole point of the compile gate: at ceiling `off` there is
            // no instrument to read, and the CLI has to say so rather than
            // print zeros.
            EXPECT_EQ( instrument.tallies().size(), noTallies );
            return;
        }

        for( const auto& step : document.steps() )
        {
            const auto name =
                grab::command_name( grab::sequence::kind_of( step.command ) );
            const auto* const entry = tally_for( instrument, name );
            ASSERT_NE( entry, nullptr ) << name;
            EXPECT_GT( entry->calls, noCalls ) << name;
            EXPECT_GE( entry->longest, entry->shortest ) << name;
        }

        // Every phase name the CLI prints, and every one of them reached.
        for( const auto name :
             { phase::playPump,
               phase::playReadyScan,
               phase::playEnter,
               phase::playTick,
               phase::playExit } )
        {
            const auto* const entry = tally_for( instrument, name );
            ASSERT_NE( entry, nullptr ) << name;
            EXPECT_GT( entry->calls, noCalls ) << name;
        }

        // pump CONTAINS the dispatches, so it can never be the smaller number.
        const auto* const pumped  = tally_for( instrument, phase::playPump );
        const auto* const entered = tally_for( instrument, phase::playEnter );
        ASSERT_NE( pumped, nullptr );
        ASSERT_NE( entered, nullptr );
        EXPECT_GE( pumped->total, entered->total );
    }

    TEST( Player,
          SummaryNamesTheLongestStepAndIsEmittedExactlyOnce )
    {
        const auto quick    = positional_id( 0U );
        const auto dominant = positional_id( 1U );
        const auto trailing = positional_id( 2U );
        auto       document = build_or_die( {
            make_step( a_click(), {}, std::string{ quickLabel } ),
            make_step( a_capture(), { quick }, std::string{ dominantLabel } ),
            make_step( a_click(), { dominant }, std::string{ trailingLabel } ),
        } );
        FakeRunner runner{ document };
        runner.script( quick ).pace    = tinySpan;
        runner.script( dominant ).pace = dominantSpan;
        runner.script( trailing ).pace = tinySpan;

        Player     player{ document, runner };
        const auto text = captured_log( grab::log::Level::Nominal,
                                        [&player]()
                                        {
                                            ASSERT_TRUE( player.play().has_value() );
                                            ( void )run_to_completion( player, origin );
                                        } );

        ASSERT_EQ( player.state(), PlayState::Done );

        // The structural truth first. The summary is only ever allowed to
        // REPORT what the run's own accessors already say, so that is what
        // pins the behaviour; the log line is checked against it.
        EXPECT_EQ( player.timing_of( dominant ).call_duration, Nanos{ dominantSpan } );
        EXPECT_GT( player.timing_of( dominant ).call_duration,
                   player.timing_of( quick ).call_duration );
        EXPECT_GT( player.timing_of( dominant ).call_duration,
                   player.timing_of( trailing ).call_duration );
        EXPECT_EQ( player.deepest_frontier(), soloFrontier );

        if constexpr( !grab::log::enabled( grab::log::Level::Nominal ) )
        {
            EXPECT_TRUE( text.empty() );
            return;
        }

        EXPECT_EQ( occurrences( text, field( "summary", sequenceName ) ), oneOccurrence )
            << text;
        EXPECT_TRUE( has_field( text, "state", doneText ) ) << text;
        EXPECT_TRUE( has_field( text, "longest_label", dominantLabel ) ) << text;
        EXPECT_NE(
            text.find( numeric_field( "longest_step",
                                      static_cast<std::size_t>( dominant.index() ) ) ),
            std::string::npos
        ) << text;
        EXPECT_NE(
            text.find( numeric_field( "longest_us",
                                      static_cast<std::size_t>( dominantSpan.count() *
                                                                microsPerMilli ) ) ),
            std::string::npos
        ) << text;
        EXPECT_NE(
            text.find( numeric_field( "deepest_frontier", soloFrontier ) ),
            std::string::npos
        ) << text;
        // An overflowed instrument must never read as a full accounting; this
        // run did not overflow, so it must say the other thing.
        EXPECT_TRUE( has_field( text, "instrument", completeText ) ) << text;
    }

    TEST( Player,
          ABlockedStepLogsWhichPredecessorItIsWaitingOn )
    {
        const auto slow     = positional_id( 0U );
        const auto other    = positional_id( 1U );
        const auto blocked  = positional_id( 2U );
        auto       document = build_or_die( {
            make_step( a_wait( longWait ), {}, std::string{ blockerLabel } ),
            make_step( a_click(), {}, std::string{ otherLabel } ),
            make_step( a_click(), { slow, other }, std::string{ blockedLabel } ),
        } );
        FakeRunner runner{ document };

        Player     player{ document, runner };
        const auto text =
            captured_log( grab::log::Level::Verbose,
                          [&player]()
                          {
                              ASSERT_TRUE( player.play().has_value() );
                              ASSERT_TRUE( player.pump( origin ).has_value() );
                          } );

        // Structural: the run really is stalled, and on that predecessor.
        EXPECT_EQ( player.status_of( other ), StepStatus::Succeeded );
        EXPECT_EQ( player.status_of( slow ), StepStatus::Running );
        EXPECT_EQ( player.status_of( blocked ), StepStatus::Pending );
        EXPECT_EQ( player.deepest_frontier(), pairedFrontier );

        if constexpr( !grab::log::enabled( grab::log::Level::Verbose ) )
        {
            EXPECT_TRUE( text.empty() );
            return;
        }

        // "Blocked" with no reason is the log line people curse at, so the
        // record has to NAME the predecessor and say what it is doing.
        EXPECT_NE(
            text.find( numeric_field( "blocked",
                                      static_cast<std::size_t>( blocked.index() ) ) ),
            std::string::npos
        ) << text;
        EXPECT_NE(
            text.find( numeric_field( "waiting_on",
                                      static_cast<std::size_t>( slow.index() ) ) ),
            std::string::npos
        ) << text;
        EXPECT_TRUE( has_field( text, "waiting_on_label", blockerLabel ) ) << text;
        EXPECT_TRUE( has_field( text, "waiting_on_status", runningText ) ) << text;
    }

    TEST( Player,
          EveryStepAndPlayStateTransitionIsLogged )
    {
        const auto first    = positional_id( 0U );
        auto       document = build_or_die( {
            make_step( a_click(), {}, std::string{ quickLabel } ),
            make_step( a_click(), { first }, std::string{ trailingLabel } ),
        } );
        FakeRunner runner{ document };

        Player     player{ document, runner };
        const auto text =
            captured_log( grab::log::Level::Verbose,
                          [&player]()
                          {
                              ASSERT_TRUE( player.play().has_value() );
                              ASSERT_TRUE( player.pump( origin ).has_value() );
                          } );

        ASSERT_EQ( player.state(), PlayState::Done );

        if constexpr( !grab::log::enabled( grab::log::Level::Verbose ) )
        {
            EXPECT_TRUE( text.empty() );
            return;
        }

        // The three transitions a step makes on the happy path, and the two
        // the run makes.
        EXPECT_TRUE( has_field( text, "to", readyText ) ) << text;
        EXPECT_TRUE( has_field( text, "from", readyText ) ) << text;
        EXPECT_TRUE( has_field( text, "to", runningText ) ) << text;
        EXPECT_TRUE( has_field( text, "from", runningText ) ) << text;
        EXPECT_TRUE( has_field( text, "to", "succeeded" ) ) << text;
        EXPECT_TRUE( has_field( text, "state_to", "playing" ) ) << text;
        EXPECT_TRUE( has_field( text, "state_to", doneText ) ) << text;
        // Admission and retirement, both named, both with the depth after.
        EXPECT_NE(
            text.find( numeric_field( "admitted",
                                      static_cast<std::size_t>( first.index() ) ) ),
            std::string::npos
        ) << text;
        EXPECT_NE(
            text.find( numeric_field( "retired",
                                      static_cast<std::size_t>( first.index() ) ) ),
            std::string::npos
        ) << text;
    }

    TEST( Player,
          GraceActuallyAppliedIsLoggedWithTheModeThatAuthorisedIt )
    {
        const auto first = positional_id( 0U );
        auto       steps = std::vector<Step>{
            make_step( a_click(), {}, std::string{ quickLabel } ),
            make_step( a_click(), { first }, std::string{ gracedLabel } ),
        };
        steps[1].extra_grace = extraGraceSpan;
        auto document        = build_or_die(
            std::move( steps ),
            PacingOptions{ .mode = PacingMode::Precise, .grace = graceSpan }
        );
        FakeRunner runner{ document };

        const auto second = positional_id( 1U );
        Player     player{ document, runner };
        const auto text =
            captured_log( grab::log::Level::Verbose,
                          [&player]()
                          {
                              ASSERT_TRUE( player.play().has_value() );
                              ASSERT_TRUE( player.pump( origin ).has_value() );
                          } );

        // Structural: the delay the log claims is the delay the run took.
        ASSERT_TRUE( player.ready_at( second ).has_value() );
        EXPECT_EQ( *player.ready_at( second ) - origin,
                   Nanos{ graceSpan } + Nanos{ extraGraceSpan } );

        if constexpr( !grab::log::enabled( grab::log::Level::Verbose ) )
        {
            EXPECT_TRUE( text.empty() );
            return;
        }

        EXPECT_NE(
            text.find( numeric_field( "graced",
                                      static_cast<std::size_t>( second.index() ) ) ),
            std::string::npos
        ) << text;
        // The mode is the SOLE authority on extra_grace, so it is logged
        // alongside the number it authorised.
        EXPECT_TRUE( has_field( text, "mode", preciseText ) ) << text;
        EXPECT_NE(
            text.find( numeric_field( "base_grace_us",
                                      static_cast<std::size_t>( graceSpan.count() *
                                                                microsPerMilli ) ) ),
            std::string::npos
        ) << text;
        EXPECT_NE(
            text.find( numeric_field( "extra_grace_us",
                                      static_cast<std::size_t>( extraGraceSpan.count() *
                                                                microsPerMilli ) ) ),
            std::string::npos
        ) << text;
        EXPECT_NE(
            text.find(
                numeric_field( "total_grace_us",
                               static_cast<std::size_t>( ( graceSpan.count() +
                                                           extraGraceSpan.count() ) *
                                                         microsPerMilli ) )
            ),
            std::string::npos
        ) << text;
    }

    TEST( Player,
          DeepestFrontierReportsRealisedParallelismRatherThanStepCount )
    {
        auto       chain = build_or_die( three_step_chain() );
        FakeRunner chainRunner{ chain };
        Player     chainPlayer{ chain, chainRunner };
        ASSERT_TRUE( chainPlayer.play().has_value() );
        ( void )run_to_completion( chainPlayer, origin );

        auto       forked = build_or_die( {
            make_step( a_drag(), {} ),
            make_step( a_capture(), {} ),
        } );
        FakeRunner forkedRunner{ forked };
        forkedRunner.script( positional_id( 0U ) ).pace = dragSpan;
        forkedRunner.script( positional_id( 1U ) ).pace = captureSpan;
        Player forkedPlayer{ forked, forkedRunner };
        ASSERT_TRUE( forkedPlayer.play().has_value() );
        ( void )run_to_completion( forkedPlayer, origin );

        ASSERT_EQ( chainPlayer.state(), PlayState::Done );
        ASSERT_EQ( forkedPlayer.state(), PlayState::Done );

        // Same number of steps in flight over time; different parallelism.
        // Every other number the Player reports is blind to the difference.
        EXPECT_EQ( chainPlayer.deepest_frontier(), soloFrontier );
        EXPECT_EQ( forkedPlayer.deepest_frontier(), pairedFrontier );
    }

    TEST( Player,
          UnwindLogsEachStepItReapedInOrder )
    {
        const auto held     = positional_id( 0U );
        const auto running  = positional_id( 1U );
        auto       document = build_or_die( {
            make_step( a_click(), {}, std::string{ quickLabel } ),
            make_step( a_wait( longWait ), { held }, std::string{ trailingLabel } ),
        } );
        FakeRunner runner{ document };
        runner.script( held ).document_hold = true;

        Player     player{ document, runner };
        const auto text =
            captured_log( grab::log::Level::Verbose,
                          [&player]()
                          {
                              ASSERT_TRUE( player.play().has_value() );
                              ASSERT_TRUE( player.pump( origin ).has_value() );
                              ASSERT_TRUE( player.interrupt().has_value() );
                          } );

        // Structural: the explicit hold really did come back up.
        EXPECT_EQ( player.neutralization(), grab::NeutralizationOutcome::Released );
        EXPECT_EQ( player.status_of( running ), StepStatus::Skipped );

        if constexpr( !grab::log::enabled( grab::log::Level::Verbose ) )
        {
            EXPECT_TRUE( text.empty() );
            return;
        }

        // Reverse entry order, so the later step is reaped first. Both are
        // named, and each says whether it released anything.
        EXPECT_NE(
            text.find( numeric_field( "unwound",
                                      static_cast<std::size_t>( running.index() ) ) ),
            std::string::npos
        ) << text;
        EXPECT_NE(
            text.find( numeric_field( "unwound",
                                      static_cast<std::size_t>( held.index() ) ) ),
            std::string::npos
        ) << text;
        EXPECT_TRUE( has_field( text, "path", "release_holds" ) ) << text;
        EXPECT_TRUE( has_field( text, "released", "true" ) ) << text;
        EXPECT_TRUE( has_field( text, "released", "false" ) ) << text;
        EXPECT_TRUE( has_field( text, "outcome", "released" ) ) << text;
        // The summary is emitted for an interrupted run too, not only a Done
        // one -- a run that aborted is exactly when someone reads it.
        EXPECT_EQ( occurrences( text, field( "summary", sequenceName ) ), oneOccurrence )
            << text;
        EXPECT_TRUE( has_field( text, "state", "interrupted" ) ) << text;
    }

}    // namespace
