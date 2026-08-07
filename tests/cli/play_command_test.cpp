// `grab play`, its flag precedence over the document, and the runner contract
// that keeps a held button from outliving the process.
//
// Everything here is DISPLAY-FREE. The seat is a template parameter precisely
// so these can run without an X connection, and the two doubles below are the
// whole reason SeatRunner is not welded to grab::Input.

#include "frontends/cli/common.hpp"
#include "frontends/cli/play_command.hpp"
#include "grab/command.hpp"
#include "grab/command_descriptor.hpp"
#include "grab/geometry/point.hpp"
#include "grab/overlay.hpp"
#include "grab/result.hpp"
#include "grab/sequence_types.hpp"
#include "grab/session.hpp"
#include "grab/trace.hpp"
#include "kernel/presentation/trail_animator.hpp"
#include "kernel/sequence/player.hpp"
#include "kernel/sequence/sequence.hpp"
#include "support/recording_seat.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace
{

    using grab::kernel::sequence::Player;
    using grab::kernel::sequence::Sequence;
    using grab::sequence::PacingMode;
    using grab::sequence::PacingOptions;
    using grab::sequence::Status;
    using grab::sequence::Step;
    using grab::sequence::StepId;

    constexpr int                       successExit    = grab::cli::successExitCode;
    constexpr int                       runtimeExit    = grab::cli::runtimeExitCode;
    constexpr int                       usageExit      = grab::cli::usageExitCode;

    constexpr std::string_view          documentPath   = "flow.json";
    constexpr std::string_view          reportPath     = "run.jsonl";
    constexpr std::string_view          pacingFlag     = "--pacing";
    constexpr std::string_view          graceFlag      = "--grace-ms";
    constexpr std::string_view          dryRunFlag     = "--dry-run";
    constexpr std::string_view          reportFlag     = "--report";
    constexpr std::string_view          traceFlag      = "--trace";
    constexpr std::string_view          preciseMode    = "precise";
    constexpr std::string_view          strictMode     = "strict";
    constexpr std::string_view          unknownMode    = "sloppy";
    constexpr std::string_view          unknownFlag    = "--turbo";
    constexpr std::string_view          notANumber     = "soon";
    constexpr std::string_view          graceValue     = "80";
    constexpr std::string_view          secondDocument = "other.json";
    constexpr std::string_view          sequenceName   = "test-flow";
    constexpr std::string_view          controlKey     = "Control_L";
    constexpr std::string_view          copyKey        = "c";
    constexpr std::string_view          typedText      = "hi";

    constexpr std::chrono::milliseconds shortWait{ 20 };
    constexpr std::chrono::milliseconds documentGrace{ 25 };
    constexpr std::chrono::milliseconds overrideGrace{ 80 };
    constexpr std::chrono::milliseconds extraGrace{ 400 };
    constexpr std::chrono::milliseconds waitDuration{ 250 };

    constexpr std::int32_t              clickX              = 640;
    constexpr std::int32_t              clickY              = 400;
    constexpr std::uint8_t              primaryButton       = 1U;
    constexpr std::size_t               clickSeatEventCount = 5U;
    constexpr std::size_t               noOutstandingHolds  = 0U;
    constexpr std::size_t               oneOutstandingHold  = 1U;
    constexpr std::size_t               oneStep             = 1U;
    constexpr std::size_t               twoSteps            = 2U;
    constexpr std::size_t               firstStep           = 0U;
    constexpr std::size_t               secondStep          = 1U;
    constexpr std::size_t               chordKeyEventCount  = 4U;
    // Seat calls, counted rather than drawn: a capture with no matching
    // release is what freezes a desktop.
    constexpr std::size_t               noCalls       = 0U;
    constexpr std::size_t               oneCall       = 1U;

    constexpr std::string_view          playVerb      = "play";
    constexpr std::string_view          sequenceLine  = "sequence: test-flow";
    constexpr std::string_view          stepsLine     = "steps: 2";
    constexpr std::string_view          orderLine     = "order: 0 1";
    constexpr std::string_view          planPrefix    = "plan: >= ";
    constexpr std::string_view          clickStepLine = "step 0 '' input.click after=[]";
    constexpr std::string_view          waitStepLine  = "step 1 '' time.wait after=[0]";
    constexpr std::string_view strictPlanLine  = "plan: >= 250 ms, 1 steps unestimated";
    constexpr std::string_view gracePlanLine   = "plan: >= 330 ms, 1 steps unestimated";
    constexpr std::string_view pointerFragment = "/steps/";
    constexpr std::string_view opFragment      = R"("op":"input.click")";
    constexpr std::string_view succeededFragment  = R"("status":"succeeded")";
    constexpr std::string_view declaredFragment   = R"("declared_ns":250000000)";
    constexpr std::string_view undeclaredFragment = R"("declared_ns":null)";
    constexpr std::string_view captureOpFragment  = "screen.capture";
    constexpr std::string_view abortFragment      = "abort";

    // ── --trace ───────────────────────────────────────────
    //
    // Section markers carry their leading newline and trailing space on
    // purpose: "  run" without them also matches ", not run" in the headline,
    // which would make the dry-run assertion pass for the wrong reason.
    constexpr std::string_view loadSection           = "\n  load";
    constexpr std::string_view runSection            = "\n  run ";
    constexpr std::string_view schedulingSection     = "\n  scheduling\n";
    constexpr std::string_view idleTimerLine         = "no deadline was ever waited on";
    constexpr std::string_view wakeLatencyLine       = "wake latency";
    constexpr std::string_view spuriousLine          = "spurious wakes";

    constexpr std::string_view traceKindFragment     = R"("kind":"trace")";
    constexpr std::string_view schedulingKeyFragment = R"("scheduling")";
    constexpr std::string_view runTalliesKeyFragment = R"("run_tallies")";
    constexpr std::string_view startKeyFragment      = R"("start_ns")";
    constexpr std::string_view waitKeyFragment       = R"("wait_ns")";
    constexpr std::string_view rootWaitFragment      = R"("wait_ns":null)";
    constexpr std::string_view endKeyFragment        = R"("end_ns")";
    constexpr std::string_view callKeyFragment       = R"("call_ns")";
    constexpr std::string_view overrunKeyFragment    = R"("overrun_ns")";
    constexpr std::string_view receiptKeyFragment    = R"("receipt")";

    constexpr std::string_view moveTallyName         = "input.move";
    constexpr std::string_view waitTallyName         = "time.wait";
    constexpr std::string_view clickTallyName        = "input.click";

    // Chosen so every rendered figure is exact and unambiguous. `waitTotalText`
    // keeps its leading space because "50.00 ms" is a substring of the run
    // total "250.00 ms" and would match it.
    constexpr std::chrono::milliseconds traceMove{ 100 };
    constexpr std::chrono::milliseconds traceWait{ 50 };
    constexpr std::chrono::milliseconds traceLoad{ 4 };
    constexpr std::chrono::milliseconds tracePlanned{ 7'010 };
    constexpr std::chrono::milliseconds traceElapsed{ 8'420 };

    constexpr std::string_view          headlineText    = "2 steps in 8.42 s";
    constexpr std::string_view          plannedText     = "planned >= 7.01 s";
    constexpr std::string_view          unestimatedText = "1 unestimated";
    constexpr std::string_view          loadTotalText   = "4.00 ms";
    constexpr std::string_view          runCountText    = "3 steps";
    constexpr std::string_view          runTotalText    = "250.00 ms";
    constexpr std::string_view          moveTotalText   = "200.00 ms";
    constexpr std::string_view          moveCountText   = "2 calls";
    constexpr std::string_view          moveMeanText    = "mean 100.00 ms";
    constexpr std::string_view          waitTotalText   = " 50.00 ms";
    constexpr std::string_view          waitCountText   = "1 call";

    // ── --trail / --feedback ──────────────────────────────
    //
    // Spelled here rather than reached for through the parser, so a rename in
    // play_command.cpp that quietly drops a flag fails a test instead of
    // passing one.
    constexpr std::string_view          trailFlag          = "--trail";
    constexpr std::string_view          feedbackFlag       = "--feedback";
    constexpr std::string_view          trailColorFlag     = "--trail-color";
    constexpr std::string_view          injectedColorFlag  = "--injected-color";
    constexpr std::string_view          fadeMsFlag         = "--fade-ms";
    constexpr std::string_view          trailWidthFlag     = "--trail-width";
    constexpr std::string_view          noClickFlag        = "--no-click";
    constexpr std::string_view          noHoldFlag         = "--no-hold";
    constexpr std::string_view          holdMsFlag         = "--hold-ms";
    constexpr std::string_view          rippleRadiusFlag   = "--ripple-radius";

    constexpr std::string_view          trailColorValue    = "00ff00";
    constexpr std::string_view          injectedColorValue = "ff0055";
    constexpr std::string_view          malformedColor     = "ZZTOP0";
    constexpr std::string_view          fadeValue          = "400";
    constexpr std::string_view          widthValue         = "5";
    constexpr std::string_view          holdValue          = "250";
    constexpr std::string_view          rippleRadiusValue  = "12";

    constexpr std::uint8_t              noChannel          = 0X00U;
    constexpr std::uint8_t              fullChannel        = 0XFFU;
    constexpr std::uint8_t              redOfInjected      = 0XFFU;
    constexpr std::uint8_t              greenOfInjected    = 0X00U;
    constexpr std::uint8_t              blueOfInjected     = 0X55U;
    constexpr std::chrono::milliseconds trailFade{ 400 };
    constexpr std::chrono::milliseconds holdThreshold{ 250 };
    constexpr float                     trailWidth   = 5.0F;
    constexpr double                    rippleRadius = 12.0;

    constexpr std::size_t               onePump      = 1U;

    [[nodiscard]]
    bool
    same_color( grab::overlay::Color left,
                grab::overlay::Color right ) noexcept
    {
        return left.r ==
               right.r &&
               left.g ==
               right.g &&
               left.b ==
               right.b &&
               left.a == right.a;
    }

    constexpr std::uint64_t    noRecordedCalls    = 0U;
    constexpr std::uint64_t    oneArm             = 1U;
    constexpr std::size_t      threeLines         = 3U;
    constexpr std::size_t      oneOverSlots       = 1U;
    constexpr std::string_view overflowNamePrefix = "phase-";
    constexpr std::string_view incompleteWarning  = "this report is INCOMPLETE";

    // Deliberately not a name any other test writes: the assertion is that
    // --dry-run leaves it absent, and a stale file would make that a lie.
    constexpr std::string_view captureOutputName =
        "grab-play-dry-run-must-not-exist.png";

    // A document whose only step names an op that does not exist, so the
    // loader rejects it with a JSON pointer rather than a shrug.
    constexpr std::string_view invalidDocument =
        R"({"steps":[{"id":"bad","op":"input.telekinesis"}]})";

    constexpr std::string_view captureDocument =
        R"({"steps":[{"id":"shot","op":"screen.capture",)"
        R"("out":"grab-play-dry-run-must-not-exist.png"}]})";

    constexpr std::string_view emptyDocument = R"({"steps":[]})";

    [[nodiscard]]
    StepId
    step_id( std::size_t index )
    {
        return StepId{ static_cast<StepId::Half>( index ), StepId::firstGeneration };
    }

    [[nodiscard]]
    std::vector<std::string_view>
    arguments( std::span<const std::string_view> values )
    {
        return std::vector<std::string_view>{ values.begin(), values.end() };
    }

    [[nodiscard]]
    bool
    contains( std::string_view haystack,
              std::string_view needle )
    {
        return haystack.find( needle ) != std::string_view::npos;
    }

    // A document on disk, removed with its directory when the test ends.
    class TempDocument
    {
        public:

            explicit TempDocument( std::string_view contents ) :
                root_( std::filesystem::temp_directory_path() / unique_name() ),
                path_( root_ / documentPath )
            {
                std::error_code error;
                static_cast<void>( std::filesystem::remove_all( root_, error ) );
                error.clear();
                const bool created = std::filesystem::create_directories( root_, error );
                EXPECT_TRUE( created );
                EXPECT_FALSE( error );

                std::ofstream stream{ path_ };
                stream << contents;
                EXPECT_TRUE( stream.good() );
            }

            ~TempDocument() noexcept
            {
                std::error_code error;
                static_cast<void>( std::filesystem::remove_all( root_, error ) );
            }

            TempDocument( const TempDocument& ) = delete;
            TempDocument&
            operator=( const TempDocument& ) = delete;
            TempDocument( TempDocument&& )   = delete;
            TempDocument&
            operator=( TempDocument&& ) = delete;

            [[nodiscard]]
            std::string
            path() const
            {
                return path_.string();
            }

            [[nodiscard]]
            std::filesystem::path
            sibling( std::string_view name ) const
            {
                return root_ / name;
            }

        private:

            [[nodiscard]]
            static std::string
            unique_name()
            {
                const auto* info = testing::UnitTest::GetInstance()->current_test_info();
                return std::string{ "grab-play-" } +
                       info->test_suite_name() +
                       "-" +
                       info->name();
            }

            std::filesystem::path root_;
            std::filesystem::path path_;
    };

    // A seat with a keyboard and a text surface, which RecordingSeat has no
    // reason to grow: it exists for the drag/pointer executor. Records what it
    // was asked to do and can be told to refuse.
    class ChordSeat final
    {
        public:

            struct ButtonEvent
            {
                    std::uint8_t code{};
                    bool         pressed{};
            };

            struct KeyEvent
            {
                    std::string name{};
                    bool        pressed{};
            };

            [[nodiscard]]
            grab::Result<void>
            move_pointer_absolute( std::int16_t x,
                                   std::int16_t y )
            {
                moves_.push_back( grab::geometry::Point{ .x = x, .y = y } );
                return {};
            }

            [[nodiscard]]
            grab::Result<void>
            button( std::uint8_t code,
                    bool         pressed )
            {
                buttons_.push_back( ButtonEvent{ .code = code, .pressed = pressed } );
                return {};
            }

            [[nodiscard]]
            grab::Result<void>
            flush()
            {
                return {};
            }

            [[nodiscard]]
            grab::Result<void>
            key_by_name( std::string_view name,
                         bool             pressed )
            {
                keys_.push_back(
                    KeyEvent{ .name = std::string{ name }, .pressed = pressed }
                );
                return {};
            }

            [[nodiscard]]
            grab::Result<void>
            type_text( std::string_view utf8 )
            {
                typed_.emplace_back( utf8 );
                if( refuse_text_ )
                {
                    return grab::fail( grab::ErrorCode::ProviderFailed,
                                       "the seat refuses to type" );
                }
                return {};
            }

            // ── OverlaySeat ──────────────────────────────────
            //
            // Only the two calls the runner has to account for are counted.
            // What freezes a desktop is a capture_pointer with no matching
            // release_pointer after it, so the assertion is a number, not a
            // drawing.

            [[nodiscard]]
            grab::Result<void>
            overlay_add( std::string_view,
                         const grab::overlay::Shape& )
            {
                return {};
            }

            [[nodiscard]]
            grab::Result<void>
            overlay_update( std::string_view,
                            const grab::overlay::Shape& )
            {
                return {};
            }

            [[nodiscard]]
            grab::Result<void>
            overlay_remove( std::string_view )
            {
                return {};
            }

            [[nodiscard]]
            grab::Result<void>
            overlay_clear()
            {
                return {};
            }

            [[nodiscard]]
            grab::Result<void>
            overlay_grab()
            {
                ++grabs_;
                if( refuse_grab_ )
                {
                    return grab::fail( grab::ErrorCode::ProviderFailed,
                                       "the seat refuses to capture the pointer" );
                }
                return {};
            }

            [[nodiscard]]
            grab::Result<void>
            overlay_release()
            {
                ++ungrabs_;
                return {};
            }

            [[nodiscard]]
            grab::Result<void>
            overlay_attach( std::string_view,
                            std::optional<grab::geometry::Point> )
            {
                return {};
            }

            [[nodiscard]]
            grab::Result<void>
            overlay_detach( std::string_view )
            {
                return {};
            }

            void
            refuse_text() noexcept
            {
                refuse_text_ = true;
            }

            // A grab that FAILS may still have been granted: the server can
            // hand the pointer over whatever the round trip reports, which is
            // why the flag is set before the call and why the failure path has
            // to release anyway.
            void
            refuse_grab() noexcept
            {
                refuse_grab_ = true;
            }

            [[nodiscard]]
            std::size_t
            grabs() const noexcept
            {
                return grabs_;
            }

            [[nodiscard]]
            std::size_t
            ungrabs() const noexcept
            {
                return ungrabs_;
            }

            [[nodiscard]]
            const std::vector<ButtonEvent>&
            buttons() const noexcept
            {
                return buttons_;
            }

            [[nodiscard]]
            const std::vector<KeyEvent>&
            keys() const noexcept
            {
                return keys_;
            }

            [[nodiscard]]
            const std::vector<std::string>&
            typed() const noexcept
            {
                return typed_;
            }

        private:

            std::vector<grab::geometry::Point> moves_{};
            std::vector<ButtonEvent>           buttons_{};
            std::vector<KeyEvent>              keys_{};
            std::vector<std::string>           typed_{};
            std::size_t                        grabs_{ 0U };
            std::size_t                        ungrabs_{ 0U };
            bool                               refuse_text_{ false };
            bool                               refuse_grab_{ false };
    };

    static_assert( grab::kernel::sequence::OverlaySeat<ChordSeat> );

    // Answers one status for every step, so the failure-to-exit-code mapping
    // is assertable without a seat at all.
    class ScriptedRunner final : public grab::kernel::sequence::CommandRunner
    {
        public:

            explicit ScriptedRunner( Status outcome ) noexcept :
                outcome_( outcome )
            {
            }

            [[nodiscard]]
            Status
            enter( const Step&,
                   std::chrono::steady_clock::time_point ) override
            {
                ++entered_;
                return outcome_;
            }

            [[nodiscard]]
            Status
            tick( const Step&,
                  std::chrono::steady_clock::time_point ) override
            {
                return outcome_;
            }

            grab::NeutralizationOutcome
            exit( const Step&,
                  std::chrono::steady_clock::time_point ) override
            {
                ++exited_;
                return grab::NeutralizationOutcome::NothingHeld;
            }

            [[nodiscard]]
            std::size_t
            entered() const noexcept
            {
                return entered_;
            }

            [[nodiscard]]
            std::size_t
            exited() const noexcept
            {
                return exited_;
            }

        private:

            Status      outcome_;
            std::size_t entered_{ 0U };
            std::size_t exited_{ 0U };
    };

    [[nodiscard]]
    Sequence
    build_or_die( std::vector<Step> steps,
                  PacingOptions     pacing )
    {
        auto program =
            Sequence::build( std::move( steps ), pacing, std::string{ sequenceName } );
        if( !program.has_value() )
        {
            ADD_FAILURE() << program.error().message;
            return Sequence{};
        }
        return std::move( *program );
    }

    // move -> wait, the smallest document with one estimated and one
    // unestimated step, which is what makes the plan's two halves visible.
    [[nodiscard]]
    Sequence
    wait_after_click( PacingOptions pacing )
    {
        std::vector<Step> steps;
        steps.push_back( Step{
            .command = grab::sequence::ClickCommand{ .button = primaryButton },
        } );
        steps.push_back( Step{
            .command     = grab::sequence::WaitCommand{ .duration = waitDuration },
            .after       = { step_id( firstStep ) },
            .extra_grace = extraGrace,
        } );
        return build_or_die( std::move( steps ), pacing );
    }

    // Two steps with a REAL ready gap between them, so drive() has something
    // to arm a deadline for. A document whose steps are all immediately ready
    // never creates a TimerThread at all, and would prove nothing about
    // scheduling.
    [[nodiscard]]
    Sequence
    click_then_click( PacingOptions pacing )
    {
        std::vector<Step> steps;
        steps.push_back( Step{
            .command = grab::sequence::ClickCommand{ .button = primaryButton },
        } );
        steps.push_back( Step{
            .command = grab::sequence::ClickCommand{ .button = primaryButton },
            .after   = { step_id( firstStep ) },
        } );
        return build_or_die( std::move( steps ), pacing );
    }

}    // namespace

TEST( PlayCommand,
      ParsesEveryFlag )
{
    const std::array values{
        documentPath,
        pacingFlag,
        preciseMode,
        graceFlag,
        graceValue,
        dryRunFlag,
        reportFlag,
        reportPath
    };
    const auto options = grab::cli::parse_play_options( arguments( values ) );

    ASSERT_TRUE( options.has_value() ) << options.error().message;
    EXPECT_EQ( options->document, documentPath );
    EXPECT_EQ( options->report, reportPath );
    EXPECT_TRUE( options->dry_run );
    ASSERT_TRUE( options->pacing.has_value() );
    EXPECT_EQ( *options->pacing, PacingMode::Precise );
    ASSERT_TRUE( options->grace.has_value() );
    EXPECT_EQ( *options->grace, overrideGrace );
}

TEST( PlayCommand,
      AbsentFlagsLeaveTheDocumentAlone )
{
    const std::array values{ documentPath };
    const auto       options = grab::cli::parse_play_options( arguments( values ) );

    ASSERT_TRUE( options.has_value() ) << options.error().message;
    EXPECT_FALSE( options->pacing.has_value() );
    EXPECT_FALSE( options->grace.has_value() );
    EXPECT_FALSE( options->dry_run );
    EXPECT_TRUE( options->report.empty() );
}

TEST( PlayCommand,
      RejectsAMissingDocument )
{
    const std::array values{ dryRunFlag };

    EXPECT_FALSE( grab::cli::parse_play_options( arguments( values ) ).has_value() );
}

TEST( PlayCommand,
      RejectsASecondDocument )
{
    const std::array values{ documentPath, secondDocument };

    EXPECT_FALSE( grab::cli::parse_play_options( arguments( values ) ).has_value() );
}

TEST( PlayCommand,
      RejectsAnUnknownFlag )
{
    const std::array values{ documentPath, unknownFlag };

    EXPECT_FALSE( grab::cli::parse_play_options( arguments( values ) ).has_value() );
}

TEST( PlayCommand,
      RejectsAnUnknownPacingMode )
{
    const std::array values{ documentPath, pacingFlag, unknownMode };

    EXPECT_FALSE( grab::cli::parse_play_options( arguments( values ) ).has_value() );
}

TEST( PlayCommand,
      RejectsANonNumericGrace )
{
    const std::array values{ documentPath, graceFlag, notANumber };

    EXPECT_FALSE( grab::cli::parse_play_options( arguments( values ) ).has_value() );
}

TEST( PlayCommand,
      RejectsAPacingFlagWithNoValue )
{
    const std::array values{ documentPath, pacingFlag };

    EXPECT_FALSE( grab::cli::parse_play_options( arguments( values ) ).has_value() );
}

// The flags win: that is what lets one document run tight or loose without
// being edited.
TEST( PlayCommand,
      FlagsOverrideTheDocumentPacing )
{
    const std::array
               values{ documentPath, pacingFlag, strictMode, graceFlag, graceValue };
    const auto options = grab::cli::parse_play_options( arguments( values ) );
    ASSERT_TRUE( options.has_value() ) << options.error().message;

    const auto pacing = grab::cli::effective_pacing(
        PacingOptions{ .mode = PacingMode::Grace, .grace = documentGrace },
        *options
    );

    EXPECT_EQ( pacing.mode, PacingMode::Strict );
    EXPECT_EQ( pacing.grace, overrideGrace );
}

// The mode and the interval override independently, so --grace-ms alone
// changes the gap without changing which mode reads it.
TEST( PlayCommand,
      TheGraceFlagAloneKeepsTheDocumentMode )
{
    const std::array values{ documentPath, graceFlag, graceValue };
    const auto       options = grab::cli::parse_play_options( arguments( values ) );
    ASSERT_TRUE( options.has_value() ) << options.error().message;

    const auto pacing = grab::cli::effective_pacing(
        PacingOptions{ .mode = PacingMode::Precise, .grace = documentGrace },
        *options
    );

    EXPECT_EQ( pacing.mode, PacingMode::Precise );
    EXPECT_EQ( pacing.grace, overrideGrace );
}

TEST( PlayCommand,
      DryRunPrintsTheOrderAndThePlan )
{
    const PacingOptions pacing{ .mode = PacingMode::Strict, .grace = overrideGrace };
    const auto          program = wait_after_click( pacing );
    const auto          text    = grab::cli::dry_run_report( program, pacing );

    EXPECT_TRUE( contains( text, sequenceLine ) ) << text;
    EXPECT_TRUE( contains( text, stepsLine ) ) << text;
    EXPECT_TRUE( contains( text, orderLine ) ) << text;
    EXPECT_TRUE( contains( text, planPrefix ) ) << text;
    EXPECT_TRUE( contains( text, clickStepLine ) ) << text;
    EXPECT_TRUE( contains( text, waitStepLine ) ) << text;
}

// The whole reason planned() grew a pacing overload: printing the document's
// figure while running under an overridden mode would be silently wrong.
TEST( PlayCommand,
      ThePlanIsRecomputedUnderTheOverriddenPacing )
{
    const PacingOptions strict{ .mode = PacingMode::Strict, .grace = overrideGrace };
    const PacingOptions grace{ .mode = PacingMode::Grace, .grace = overrideGrace };
    const auto          program     = wait_after_click( strict );

    const auto          strict_text = grab::cli::dry_run_report( program, strict );
    const auto          grace_text  = grab::cli::dry_run_report( program, grace );

    EXPECT_TRUE( contains( strict_text, strictPlanLine ) ) << strict_text;
    EXPECT_TRUE( contains( grace_text, gracePlanLine ) ) << grace_text;
    EXPECT_NE( strict_text, grace_text );
}

// with_pacing rebuilds the document, and ids are positional, so every StepId
// survives the rebuild -- the same document is the same steps in all three
// modes.
TEST( PlayCommand,
      RepacingPreservesEveryStepId )
{
    const auto program = wait_after_click(
        PacingOptions{ .mode = PacingMode::Strict, .grace = documentGrace }
    );
    auto repaced = grab::cli::with_pacing(
        program,
        PacingOptions{ .mode = PacingMode::Precise, .grace = overrideGrace }
    );

    ASSERT_TRUE( repaced.has_value() ) << repaced.error().message;
    EXPECT_EQ( repaced->pacing().mode, PacingMode::Precise );
    EXPECT_EQ( repaced->pacing().grace, overrideGrace );
    ASSERT_EQ( repaced->steps().size(), program.steps().size() );
    for( std::size_t index = 0U; index < program.steps().size(); ++index )
    {
        EXPECT_EQ( repaced->steps()[index].id, program.steps()[index].id );
    }
}

// --dry-run is the corpus harness's entry point: a rejected document must exit
// non-zero and say where, pointer included.
TEST( PlayCommand,
      DryRunOnARejectedDocumentExitsNonZero )
{
    const TempDocument document{ invalidDocument };
    const std::string  path = document.path();
    const std::array   values{ std::string_view{ path }, dryRunFlag };

    testing::internal::CaptureStderr();
    const int         code    = grab::cli::run_play_command( arguments( values ) );
    const std::string printed = testing::internal::GetCapturedStderr();

    EXPECT_NE( code, successExit );
    EXPECT_EQ( code, runtimeExit );
    EXPECT_TRUE( contains( printed, pointerFragment ) ) << printed;
    EXPECT_TRUE( contains( printed, documentPath ) ) << printed;
}

TEST( PlayCommand,
      AMalformedCommandLineIsAUsageError )
{
    const std::array values{ documentPath, unknownFlag };

    testing::internal::CaptureStderr();
    const int code = grab::cli::run_play_command( arguments( values ) );
    static_cast<void>( testing::internal::GetCapturedStderr() );

    EXPECT_EQ( code, usageExit );
}

// It parses, validates and prints -- and touches nothing. The capture step
// would have written a PNG had anything run.
TEST( PlayCommand,
      DryRunExecutesNothing )
{
    const TempDocument document{ captureDocument };
    const std::string  path = document.path();
    // The document names a relative output, so an execution would write it
    // beside the process rather than beside the document.
    const auto         output =
        std::filesystem::current_path() / std::filesystem::path{ captureOutputName };
    std::error_code removal;
    static_cast<void>( std::filesystem::remove( output, removal ) );
    ASSERT_FALSE( std::filesystem::exists( output ) );

    const std::array values{ std::string_view{ path }, dryRunFlag };

    testing::internal::CaptureStdout();
    const int         code    = grab::cli::run_play_command( arguments( values ) );
    const std::string printed = testing::internal::GetCapturedStdout();

    EXPECT_EQ( code, successExit );
    EXPECT_TRUE( contains( printed, captureOpFragment ) ) << printed;
    EXPECT_FALSE( std::filesystem::exists( output ) );
}

// The forcing function: `grab click --at` builds this one-step document and
// plays it, and the seat must see the same warp-then-press-then-release it saw
// when the verb called grab::Input directly.
TEST( PlayCommand,
      ClickRoutesTheSameSeatTrafficThroughTheCommandLayer )
{
    auto program = grab::cli::single_step_sequence( grab::sequence::ClickAtCommand{
        .at     = grab::geometry::Point{ .x = clickX, .y = clickY },
        .button = primaryButton,
    } );
    ASSERT_TRUE( program.has_value() ) << program.error().message;

    grab::testing::RecordingSeat                        seat;
    grab::cli::SeatRunner<grab::testing::RecordingSeat> runner{ seat };
    Player                                              player{ *program, runner };

    const auto outcome = grab::cli::drive( player );

    ASSERT_TRUE( outcome.has_value() ) << outcome.error().message;
    EXPECT_EQ( player.state(), grab::sequence::PlayState::Done );
    ASSERT_EQ( seat.events().size(), clickSeatEventCount );

    using Kind = grab::testing::SeatEvent::Kind;
    EXPECT_EQ( seat.events()[0U].kind, Kind::Move );
    EXPECT_EQ( seat.events()[0U].x, clickX );
    EXPECT_EQ( seat.events()[0U].y, clickY );
    EXPECT_EQ( seat.events()[1U].kind, Kind::Flush );
    EXPECT_EQ( seat.events()[2U].kind, Kind::Button );
    EXPECT_EQ( seat.events()[2U].button, primaryButton );
    EXPECT_TRUE( seat.events()[2U].pressed );
    EXPECT_EQ( seat.events()[3U].kind, Kind::Button );
    EXPECT_FALSE( seat.events()[3U].pressed );
    EXPECT_EQ( seat.events()[4U].kind, Kind::Flush );
    EXPECT_EQ( runner.outstanding_holds(), noOutstandingHolds );
}

// drive() is the one place in this design that reads a real clock, and it must
// wait on the timer thread rather than spin: a time.wait has to cost its
// duration in wall time and reach Done on its own.
TEST( PlayCommand,
      AWaitIsPacedByTheTimerAndCompletesOnItsOwn )
{
    std::vector<Step> steps;
    steps.push_back( Step{
        .command = grab::sequence::WaitCommand{ .duration = shortWait },
    } );
    const auto program = build_or_die( std::move( steps ), PacingOptions{} );

    grab::testing::RecordingSeat                        seat;
    grab::cli::SeatRunner<grab::testing::RecordingSeat> runner{ seat };
    Player                                              player{ program, runner };

    const auto begun   = std::chrono::steady_clock::now();
    const auto outcome = grab::cli::drive( player );
    const auto spent   = std::chrono::steady_clock::now() - begun;

    ASSERT_TRUE( outcome.has_value() ) << outcome.error().message;
    EXPECT_EQ( player.state(), grab::sequence::PlayState::Done );
    EXPECT_GE( spent, shortWait );
    EXPECT_GE( player.elapsed(), shortWait );
    EXPECT_TRUE( seat.events().empty() );
}

TEST( PlayCommand,
      ASucceedingRunExitsZero )
{
    const auto program = wait_after_click(
        PacingOptions{ .mode = PacingMode::Strict, .grace = documentGrace }
    );
    ScriptedRunner runner{ Status::Success };

    const int      code =
        grab::cli::play_program( program, runner, grab::cli::PlayOptions{} );

    EXPECT_EQ( code, successExit );
    EXPECT_EQ( runner.entered(), twoSteps );
    EXPECT_EQ( runner.exited(), twoSteps );
}

// Abort is the default policy, and a run that aborts must not report success.
TEST( PlayCommand,
      AFailingStepUnderAbortExitsNonZero )
{
    const auto program = wait_after_click(
        PacingOptions{ .mode = PacingMode::Strict, .grace = documentGrace }
    );
    ScriptedRunner runner{ Status::Failure };

    testing::internal::CaptureStderr();
    const int code =
        grab::cli::play_program( program, runner, grab::cli::PlayOptions{} );
    const std::string printed = testing::internal::GetCapturedStderr();

    EXPECT_EQ( code, runtimeExit );
    EXPECT_EQ( runner.entered(), oneStep );
    EXPECT_TRUE( contains( printed, abortFragment ) ) << printed;
}

TEST( PlayCommand,
      TheReportCarriesOneRecordPerStep )
{
    const TempDocument document{ emptyDocument };
    const auto         report  = document.sibling( reportPath );
    const auto         program = wait_after_click(
        PacingOptions{ .mode = PacingMode::Strict, .grace = documentGrace }
    );
    ScriptedRunner         runner{ Status::Success };
    grab::cli::PlayOptions options;
    options.report = report.string();

    const int code = grab::cli::play_program( program, runner, options );
    EXPECT_EQ( code, successExit );

    std::ifstream stream{ report };
    ASSERT_TRUE( stream.good() );
    std::vector<std::string> lines;
    std::string              line;
    while( std::getline( stream, line ) )
    {
        lines.push_back( line );
    }

    ASSERT_EQ( lines.size(), twoSteps );
    EXPECT_TRUE( contains( lines[firstStep], opFragment ) ) << lines[firstStep];
    EXPECT_TRUE( contains( lines[firstStep], succeededFragment ) ) << lines[firstStep];
    EXPECT_TRUE( contains( lines[secondStep], declaredFragment ) ) << lines[secondStep];
    EXPECT_TRUE( contains( lines[firstStep], undeclaredFragment ) ) << lines[firstStep];
}

// A chord is the reason an explicit hold survives exit(): input.key_down is
// supposed to leave the key down for input.key_up. Nothing may be released
// twice, and nothing may be left down.
TEST( PlayCommand,
      AChordReleasesExactlyOnce )
{
    std::vector<Step> steps;
    steps.push_back( Step{
        .command = grab::sequence::KeyDownCommand{ .key = std::string{ controlKey } },
    } );
    steps.push_back( Step{
        .command = grab::sequence::KeyCommand{ .key = std::string{ copyKey } },
        .after   = { step_id( firstStep ) },
    } );
    steps.push_back( Step{
        .command = grab::sequence::KeyUpCommand{ .key = std::string{ controlKey } },
        .after   = { step_id( secondStep ) },
    } );
    const auto program = build_or_die( std::move( steps ), PacingOptions{} );

    ChordSeat  seat;
    grab::cli::SeatRunner<ChordSeat> runner{ seat };
    Player                           player{ program, runner };

    const auto                       outcome = grab::cli::drive( player );
    ASSERT_TRUE( outcome.has_value() ) << outcome.error().message;
    EXPECT_EQ( player.state(), grab::sequence::PlayState::Done );

    ASSERT_EQ( seat.keys().size(), chordKeyEventCount );
    EXPECT_EQ( seat.keys()[0U].name, controlKey );
    EXPECT_TRUE( seat.keys()[0U].pressed );
    EXPECT_EQ( seat.keys()[1U].name, copyKey );
    EXPECT_TRUE( seat.keys()[1U].pressed );
    EXPECT_EQ( seat.keys()[2U].name, copyKey );
    EXPECT_FALSE( seat.keys()[2U].pressed );
    EXPECT_EQ( seat.keys()[3U].name, controlKey );
    EXPECT_FALSE( seat.keys()[3U].pressed );

    EXPECT_EQ( runner.outstanding_holds(), noOutstandingHolds );
    EXPECT_EQ( runner.release_outstanding(), grab::NeutralizationOutcome::NothingHeld );
    EXPECT_EQ( seat.keys().size(), chordKeyEventCount );
}

// The process is the last step. A document that presses and never releases
// leaves a button down for the next application unless the runner lifts it.
TEST( PlayCommand,
      APressWithNoReleaseIsLiftedWhenTheRunEnds )
{
    std::vector<Step> steps;
    steps.push_back( Step{
        .command = grab::sequence::PressCommand{ .button = primaryButton },
    } );
    const auto program = build_or_die( std::move( steps ), PacingOptions{} );

    ChordSeat  seat;
    grab::cli::SeatRunner<ChordSeat> runner{ seat };
    Player                           player{ program, runner };

    const auto                       outcome = grab::cli::drive( player );
    ASSERT_TRUE( outcome.has_value() ) << outcome.error().message;
    ASSERT_EQ( seat.buttons().size(), oneStep );
    EXPECT_TRUE( seat.buttons()[0U].pressed );
    EXPECT_EQ( runner.outstanding_holds(), oneOutstandingHold );

    EXPECT_EQ( runner.release_outstanding(), grab::NeutralizationOutcome::Released );
    ASSERT_EQ( seat.buttons().size(), twoSteps );
    EXPECT_EQ( seat.buttons()[1U].code, primaryButton );
    EXPECT_FALSE( seat.buttons()[1U].pressed );
}

// Player::unwind skips a step it has already exited, and a SUCCESSFUL
// input.press was exited by succeed(). Its hold used to survive the abort
// entirely and be lifted only by release_outstanding(), on the way out of the
// process. release_holds() closes that: the unwind itself reaches back into
// the completed step and lifts what the document left down, because the
// input.release that was supposed to lift it has just been cancelled.
TEST( PlayCommand,
      AnAbortAfterASuccessfulPressReleasesTheButtonDuringTheUnwind )
{
    std::vector<Step> steps;
    steps.push_back( Step{
        .command = grab::sequence::PressCommand{ .button = primaryButton },
    } );
    steps.push_back( Step{
        .command = grab::sequence::TypeCommand{ .text = std::string{ typedText } },
        .after   = { step_id( firstStep ) },
    } );
    const auto program = build_or_die( std::move( steps ), PacingOptions{} );

    ChordSeat  seat;
    seat.refuse_text();
    grab::cli::SeatRunner<ChordSeat> runner{ seat };
    Player                           player{ program, runner };

    const auto                       outcome = grab::cli::drive( player );

    ASSERT_FALSE( outcome.has_value() );
    EXPECT_EQ( player.state(), grab::sequence::PlayState::Interrupted );
    ASSERT_EQ( seat.typed().size(), oneStep );

    // The unwind lifted it, so nothing is outstanding by the time the process
    // gets its turn.
    ASSERT_EQ( seat.buttons().size(), twoSteps );
    EXPECT_EQ( seat.buttons()[1U].code, primaryButton );
    EXPECT_FALSE( seat.buttons()[1U].pressed );
    EXPECT_EQ( runner.outstanding_holds(), noOutstandingHolds );
    EXPECT_EQ( player.neutralization(), grab::NeutralizationOutcome::Released );

    // And the backstop must not press it up a SECOND time: a duplicate release
    // is a duplicate event the application sees.
    EXPECT_EQ( runner.release_outstanding(), grab::NeutralizationOutcome::NothingHeld );
    EXPECT_EQ( seat.buttons().size(), twoSteps );
}

// The mirror hazard, and the one the live run caught. `state.document_hold`
// says a step TOOK an explicit hold; nothing on the success path clears it,
// because exit() must not break a chord. So a COMPLETED press/release pair
// still carries the flag, and an unwind that trusted it alone would press the
// button up a second time -- a spurious event the application sees, from a
// button that has been up for seconds.
TEST( PlayCommand,
      AnAbortDoesNotReReleaseAPressTheDocumentAlreadyReleased )
{
    std::vector<Step> steps;
    steps.push_back( Step{
        .command = grab::sequence::PressCommand{ .button = primaryButton },
    } );
    steps.push_back( Step{
        .command = grab::sequence::ReleaseCommand{ .button = primaryButton },
        .after   = { step_id( firstStep ) },
    } );
    steps.push_back( Step{
        .command = grab::sequence::TypeCommand{ .text = std::string{ typedText } },
        .after   = { step_id( secondStep ) },
    } );
    const auto program = build_or_die( std::move( steps ), PacingOptions{} );

    ChordSeat  seat;
    seat.refuse_text();
    grab::cli::SeatRunner<ChordSeat> runner{ seat };
    Player                           player{ program, runner };

    const auto                       outcome = grab::cli::drive( player );

    ASSERT_FALSE( outcome.has_value() );
    EXPECT_EQ( player.state(), grab::sequence::PlayState::Interrupted );
    // Exactly the down and the up the document asked for, and nothing else.
    ASSERT_EQ( seat.buttons().size(), twoSteps );
    EXPECT_TRUE( seat.buttons()[0U].pressed );
    EXPECT_FALSE( seat.buttons()[1U].pressed );
    EXPECT_EQ( runner.outstanding_holds(), noOutstandingHolds );
    EXPECT_EQ( runner.release_outstanding(), grab::NeutralizationOutcome::NothingHeld );
    EXPECT_EQ( seat.buttons().size(), twoSteps );
}

// The same for a chord: key_down / key / key_up completed, so the unwind must
// not lift the modifier again.
TEST( PlayCommand,
      AnAbortDoesNotReReleaseACompletedChordsModifier )
{
    constexpr std::size_t thirdStep = 2U;

    std::vector<Step>     steps;
    steps.push_back( Step{
        .command = grab::sequence::KeyDownCommand{ .key = std::string{ controlKey } },
    } );
    steps.push_back( Step{
        .command = grab::sequence::KeyCommand{ .key = std::string{ copyKey } },
        .after   = { step_id( firstStep ) },
    } );
    steps.push_back( Step{
        .command = grab::sequence::KeyUpCommand{ .key = std::string{ controlKey } },
        .after   = { step_id( secondStep ) },
    } );
    steps.push_back( Step{
        .command = grab::sequence::TypeCommand{ .text = std::string{ typedText } },
        .after   = { step_id( thirdStep ) },
    } );
    const auto program = build_or_die( std::move( steps ), PacingOptions{} );

    ChordSeat  seat;
    seat.refuse_text();
    grab::cli::SeatRunner<ChordSeat> runner{ seat };
    Player                           player{ program, runner };

    const auto                       outcome = grab::cli::drive( player );

    ASSERT_FALSE( outcome.has_value() );
    EXPECT_EQ( seat.keys().size(), chordKeyEventCount );
    EXPECT_EQ( seat.keys()[3U].name, controlKey );
    EXPECT_FALSE( seat.keys()[3U].pressed );
    EXPECT_EQ( runner.outstanding_holds(), noOutstandingHolds );
    EXPECT_EQ( runner.release_outstanding(), grab::NeutralizationOutcome::NothingHeld );
    EXPECT_EQ( seat.keys().size(), chordKeyEventCount );
}

// MECHANISM 2 of design §3.2. overlay.grab is Instant, so it always succeeds,
// so succeed() always exits it, so the Player's unwind never revisits it --
// and a run that simply REACHES THE END with the capture still taken is not
// unwound at all. Nothing but the runner's own tracking lifts it, and a
// pointer grab that outlives its owner freezes the whole desktop.
TEST( PlayCommand,
      AGrabWithNoReleaseIsLiftedWhenTheRunEnds )
{
    std::vector<Step> steps;
    steps.push_back( Step{
        .command = grab::sequence::OverlayGrabCommand{},
    } );
    const auto program = build_or_die( std::move( steps ), PacingOptions{} );

    ChordSeat  seat;
    grab::cli::SeatRunner<ChordSeat> runner{ seat };
    Player                           player{ program, runner };

    const auto                       outcome = grab::cli::drive( player );

    ASSERT_TRUE( outcome.has_value() ) << outcome.error().message;
    // Done, not Interrupted: this is the path no unwind reaches.
    EXPECT_EQ( player.state(), grab::sequence::PlayState::Done );
    EXPECT_EQ( seat.grabs(), oneCall );
    EXPECT_EQ( seat.ungrabs(), noCalls );
    EXPECT_TRUE( runner.capture_outstanding() );
    EXPECT_EQ( runner.outstanding_holds(), oneOutstandingHold );

    EXPECT_EQ( runner.release_outstanding(), grab::NeutralizationOutcome::Released );
    EXPECT_EQ( seat.ungrabs(), oneCall );
    EXPECT_FALSE( runner.capture_outstanding() );
}

// MECHANISM 1. The capture is taken, a later step aborts, and the
// overlay.release that was going to lift it never runs. release_holds() has to
// reach the already-exited grab step during the unwind -- while the run is
// still tearing down, not after the report has been written.
TEST( PlayCommand,
      AnAbortAfterASuccessfulGrabReleasesThePointerCapture )
{
    std::vector<Step> steps;
    steps.push_back( Step{
        .command = grab::sequence::OverlayGrabCommand{},
    } );
    steps.push_back( Step{
        .command = grab::sequence::TypeCommand{ .text = std::string{ typedText } },
        .after   = { step_id( firstStep ) },
    } );
    const auto program = build_or_die( std::move( steps ), PacingOptions{} );

    ChordSeat  seat;
    seat.refuse_text();
    grab::cli::SeatRunner<ChordSeat> runner{ seat };
    Player                           player{ program, runner };

    const auto                       outcome = grab::cli::drive( player );

    ASSERT_FALSE( outcome.has_value() );
    EXPECT_EQ( player.state(), grab::sequence::PlayState::Interrupted );
    EXPECT_EQ( seat.grabs(), oneCall );
    EXPECT_EQ( seat.ungrabs(), oneCall );
    EXPECT_FALSE( runner.capture_outstanding() );
    EXPECT_EQ( runner.outstanding_holds(), noOutstandingHolds );
    EXPECT_EQ( player.neutralization(), grab::NeutralizationOutcome::Released );

    EXPECT_EQ( runner.release_outstanding(), grab::NeutralizationOutcome::NothingHeld );
    EXPECT_EQ( seat.ungrabs(), oneCall );
}

// A capture that WAS released must not report NothingHeld. The server can hand
// this process the pointer whatever the round trip answers, so overlay.grab
// marks itself held before the call and exit() releases it on the unwind --
// and exit() has to count that flag when it decides what it neutralized, or
// the run reports it held nothing while it was in fact holding the desktop.
TEST( PlayCommand,
      AFailedGrabReportsThePointerCaptureAsReleased )
{
    std::vector<Step> steps;
    steps.push_back( Step{
        .command = grab::sequence::OverlayGrabCommand{},
    } );
    const auto program = build_or_die( std::move( steps ), PacingOptions{} );

    ChordSeat  seat;
    seat.refuse_grab();
    grab::cli::SeatRunner<ChordSeat> runner{ seat };
    Player                           player{ program, runner };

    const auto                       outcome = grab::cli::drive( player );

    ASSERT_FALSE( outcome.has_value() );
    EXPECT_EQ( seat.grabs(), oneCall );
    EXPECT_EQ( seat.ungrabs(), oneCall );
    EXPECT_EQ( player.neutralization(), grab::NeutralizationOutcome::Released );
    // A grab commits no input, so it is NOT PossiblyCommitted -- that verdict
    // belongs to a half-finished button press.
    EXPECT_EQ( runner.last_error( program.steps()[firstStep] ),
               grab::ErrorCode::ProviderFailed );
    EXPECT_FALSE( runner.capture_outstanding() );
}

// The pair, run through cleanly: the document lifts its own capture and
// nothing is outstanding afterwards. Without this the two tests above would be
// satisfied by a runner that released the pointer on every path, which would
// break every carry.
TEST( PlayCommand,
      AGrabFollowedByItsReleaseLeavesNothingOutstanding )
{
    std::vector<Step> steps;
    steps.push_back( Step{
        .command = grab::sequence::OverlayGrabCommand{},
    } );
    steps.push_back( Step{
        .command = grab::sequence::OverlayReleaseCommand{},
        .after   = { step_id( firstStep ) },
    } );
    const auto program = build_or_die( std::move( steps ), PacingOptions{} );

    ChordSeat  seat;
    grab::cli::SeatRunner<ChordSeat> runner{ seat };
    Player                           player{ program, runner };

    const auto                       outcome = grab::cli::drive( player );

    ASSERT_TRUE( outcome.has_value() ) << outcome.error().message;
    EXPECT_EQ( player.state(), grab::sequence::PlayState::Done );
    EXPECT_EQ( seat.grabs(), oneCall );
    EXPECT_EQ( seat.ungrabs(), oneCall );
    EXPECT_FALSE( runner.capture_outstanding() );
    EXPECT_EQ( runner.release_outstanding(), grab::NeutralizationOutcome::NothingHeld );
    EXPECT_EQ( seat.ungrabs(), oneCall );
}

// A seat missing the capability a step needs is a configuration fault, not a
// transient one, so it must not be dressed up as a provider failure the
// descriptor's RetryClass might retry.
TEST( PlayCommand,
      AMissingCapabilityIsReportedAsSuch )
{
    std::vector<Step> steps;
    steps.push_back( Step{
        .command = grab::sequence::TypeCommand{ .text = std::string{ typedText } },
    } );
    const auto program = build_or_die( std::move( steps ), PacingOptions{} );

    // RecordingSeat is a pointer seat and nothing else.
    grab::testing::RecordingSeat                        seat;
    grab::cli::SeatRunner<grab::testing::RecordingSeat> runner{ seat };
    Player                                              player{ program, runner };

    const auto outcome = grab::cli::drive( player );

    ASSERT_FALSE( outcome.has_value() );
    EXPECT_EQ( outcome.error().code, grab::ErrorCode::CapabilityUnavailable );
    EXPECT_EQ( runner.last_error( program.steps()[firstStep] ),
               grab::ErrorCode::CapabilityUnavailable );
}

// The verb still resolves from the descriptor table, which is what makes it
// reachable at all.
TEST( PlayCommand,
      ThePlayVerbResolves )
{
    const auto* const descriptor = grab::cli::find_command_by_verb( playVerb );

    ASSERT_NE( descriptor, nullptr );
    EXPECT_EQ( descriptor->kind, grab::CommandKind::Play );
}

// Three of the four input verbs route; drag-curve cannot, because there is no
// DragCurveCommand among the fifteen alternatives.
TEST( PlayCommand,
      DragCurveHasNoSequenceCommand )
{
    EXPECT_FALSE( grab::sequence::is_sequence_command( grab::CommandKind::DragCurve ) );
    EXPECT_TRUE( grab::sequence::is_sequence_command( grab::CommandKind::ClickAt ) );
    EXPECT_TRUE( grab::sequence::is_sequence_command( grab::CommandKind::Type ) );
    EXPECT_TRUE( grab::sequence::is_sequence_command( grab::CommandKind::Drag ) );
}

// ── --trace ────────────────────────────────────────────────

TEST( PlayCommand,
      TraceIsAFlagAndIsOffUnlessAskedFor )
{
    const std::array bare{ documentPath };
    const auto       plain = grab::cli::parse_play_options( arguments( bare ) );
    ASSERT_TRUE( plain.has_value() ) << plain.error().message;
    EXPECT_FALSE( plain->trace );

    const std::array asked{ documentPath, traceFlag };
    const auto       traced = grab::cli::parse_play_options( arguments( asked ) );
    ASSERT_TRUE( traced.has_value() ) << traced.error().message;
    EXPECT_TRUE( traced->trace );
}

// The report is a projection of the tallies and must not be able to disagree
// with them: the section total is the sum of its lines, the per-line mean is
// the total over the calls, and the order is by total descending so the
// expensive name is the first one read.
TEST( PlayCommand,
      TheTraceReportTotalsAgreeWithItsTallies )
{
    grab::cli::RunTrace trace;
    trace.sequence    = std::string{ sequenceName };
    trace.steps       = twoSteps;
    trace.ran         = true;
    trace.elapsed     = traceElapsed;
    trace.planned     = tracePlanned;
    trace.unestimated = oneStep;
    trace.load        = traceLoad;
    trace.run.record( moveTallyName, traceMove );
    trace.run.record( moveTallyName, traceMove );
    trace.run.record( waitTallyName, traceWait );

    ASSERT_EQ( trace.run.total(), traceMove + traceMove + traceWait );

    const auto text = grab::cli::trace_report( trace );

    EXPECT_TRUE( contains( text, sequenceName ) ) << text;
    EXPECT_TRUE( contains( text, headlineText ) ) << text;
    EXPECT_TRUE( contains( text, plannedText ) ) << text;
    EXPECT_TRUE( contains( text, unestimatedText ) ) << text;

    EXPECT_TRUE( contains( text, loadSection ) ) << text;
    EXPECT_TRUE( contains( text, loadTotalText ) ) << text;

    EXPECT_TRUE( contains( text, runSection ) ) << text;
    EXPECT_TRUE( contains( text, runCountText ) ) << text;
    EXPECT_TRUE( contains( text, runTotalText ) ) << text;

    EXPECT_TRUE( contains( text, moveCountText ) ) << text;
    EXPECT_TRUE( contains( text, moveTotalText ) ) << text;
    EXPECT_TRUE( contains( text, moveMeanText ) ) << text;
    EXPECT_TRUE( contains( text, waitCountText ) ) << text;
    EXPECT_TRUE( contains( text, waitTotalText ) ) << text;

    // Sorted by total descending: 200 ms of moves outranks 50 ms of waiting.
    EXPECT_LT( text.find( moveTallyName ), text.find( waitTallyName ) ) << text;

    // No timer was ever armed here, and the section says so rather than
    // printing zeroes that read like measurements.
    EXPECT_TRUE( contains( text, schedulingSection ) ) << text;
    EXPECT_TRUE( contains( text, idleTimerLine ) ) << text;
}

// An instrument that ran out of slots stopped recording. A report that omits
// the expensive thing while looking complete is the one failure mode worse
// than no report at all.
TEST( PlayCommand,
      AnOverflowedInstrumentIsDeclaredInTheReport )
{
    grab::cli::RunTrace trace;
    trace.ran = true;

    // One more distinct name than the instrument has slots. The storage has to
    // be distinct per name and stable while it is recorded: the instrument
    // compares by POINTER first, so reusing one buffer would fold every record
    // into a single slot and never overflow at all. Reserved up front, then
    // recorded in a second pass, because a vector reallocation would move the
    // small-string buffers the views point at.
    std::vector<std::string> names;
    names.reserve( grab::diag::maxInstrumentSlots + oneOverSlots );
    for( std::size_t slot = firstStep; slot <= grab::diag::maxInstrumentSlots; ++slot )
    {
        names.push_back( std::string{ overflowNamePrefix } + std::to_string( slot ) );
    }
    for( const auto& name : names )
    {
        trace.run.record( name, traceWait );
    }
    ASSERT_TRUE( trace.run.overflowed() );

    EXPECT_TRUE( contains( grab::cli::trace_report( trace ), incompleteWarning ) );
}

// --dry-run played nothing, so a `run` section would be a table of zeroes
// pretending to be measurements. The load DID happen and is reported.
TEST( PlayCommand,
      TraceOnADryRunReportsTheLoadAndNoRun )
{
    const TempDocument document{ captureDocument };
    const std::string  path = document.path();
    const std::array   values{ std::string_view{ path }, dryRunFlag, traceFlag };

    testing::internal::CaptureStdout();
    const int         code    = grab::cli::run_play_command( arguments( values ) );
    const std::string printed = testing::internal::GetCapturedStdout();

    EXPECT_EQ( code, successExit );
    EXPECT_TRUE( contains( printed, loadSection ) ) << printed;
    EXPECT_FALSE( contains( printed, runSection ) ) << printed;
    EXPECT_TRUE( contains( printed, schedulingSection ) ) << printed;
    EXPECT_TRUE( contains( printed, idleTimerLine ) ) << printed;
}

// Every tally comes from timing_of(), which is the same source the JSONL has
// always used -- so the pretty report and the machine-readable one cannot
// drift apart.
TEST( PlayCommand,
      ARunTraceCountsEveryStepItPlayedAndTheDeadlinesItWaitedOn )
{
    const auto program = click_then_click(
        PacingOptions{ .mode = PacingMode::Grace, .grace = documentGrace }
    );
    ScriptedRunner         runner{ Status::Success };
    grab::cli::PlayOptions options;
    options.trace = true;

    grab::cli::RunTrace trace;
    const int code = grab::cli::play_program( program, runner, options, &trace );

    EXPECT_EQ( code, successExit );
    EXPECT_TRUE( trace.ran );

    std::uint64_t calls = noRecordedCalls;
    for( const auto& tally : trace.run.tallies() )
    {
        calls += tally.calls;
    }
    EXPECT_EQ( calls, twoSteps );
    EXPECT_FALSE( trace.run.overflowed() );

    // A 25 ms ready gap is a deadline, and a deadline is armed.
    EXPECT_GE( trace.schedule.arms, oneArm );

    const auto text = grab::cli::trace_report( trace );
    EXPECT_TRUE( contains( text, clickTallyName ) ) << text;
    EXPECT_TRUE( contains( text, spuriousLine ) ) << text;
    EXPECT_TRUE( contains( text, wakeLatencyLine ) ) << text;
}

// The corpus harness is written against this record. Every key it had is
// still there, in the same shape, and the new ones sit beside them.
TEST( PlayCommand,
      TheReportGainsPerStepTimingWithoutLosingAnyExistingField )
{
    const TempDocument document{ emptyDocument };
    const auto         report  = document.sibling( reportPath );
    const auto         program = wait_after_click(
        PacingOptions{ .mode = PacingMode::Strict, .grace = documentGrace }
    );
    ScriptedRunner         runner{ Status::Success };
    grab::cli::PlayOptions options;
    options.report = report.string();

    EXPECT_EQ( grab::cli::play_program( program, runner, options ), successExit );

    std::ifstream stream{ report };
    ASSERT_TRUE( stream.good() );
    std::vector<std::string> lines;
    std::string              line;
    while( std::getline( stream, line ) )
    {
        lines.push_back( line );
    }

    // Still one line per step: a consumer counting them keeps counting them.
    ASSERT_EQ( lines.size(), twoSteps );
    EXPECT_TRUE( contains( lines[firstStep], opFragment ) ) << lines[firstStep];
    EXPECT_TRUE( contains( lines[firstStep], succeededFragment ) ) << lines[firstStep];
    EXPECT_TRUE( contains( lines[firstStep], callKeyFragment ) ) << lines[firstStep];
    EXPECT_TRUE( contains( lines[firstStep], overrunKeyFragment ) ) << lines[firstStep];
    EXPECT_TRUE( contains( lines[firstStep], receiptKeyFragment ) ) << lines[firstStep];
    EXPECT_TRUE( contains( lines[secondStep], declaredFragment ) ) << lines[secondStep];

    EXPECT_TRUE( contains( lines[firstStep], startKeyFragment ) ) << lines[firstStep];
    EXPECT_TRUE( contains( lines[firstStep], waitKeyFragment ) ) << lines[firstStep];
    EXPECT_TRUE( contains( lines[firstStep], endKeyFragment ) ) << lines[firstStep];

    // A root is admitted by play(), which reads no clock, so it has no ready
    // instant to have waited from. Reporting one would be a span measured from
    // the steady clock's epoch -- days, dressed as a scheduling cost.
    EXPECT_TRUE( contains( lines[firstStep], rootWaitFragment ) ) << lines[firstStep];

    // Without --trace there is no summary line, so the one-line-per-step
    // invariant is unconditional for anything written before this change.
    EXPECT_FALSE( contains( lines[firstStep], traceKindFragment ) ) << lines[firstStep];
    EXPECT_FALSE( contains( lines[secondStep], traceKindFragment ) )
        << lines[secondStep];
}

TEST( PlayCommand,
      TraceAddsExactlyOneSummaryLineToTheReport )
{
    const TempDocument document{ emptyDocument };
    const auto         report  = document.sibling( reportPath );
    const auto         program = wait_after_click(
        PacingOptions{ .mode = PacingMode::Strict, .grace = documentGrace }
    );
    ScriptedRunner         runner{ Status::Success };
    grab::cli::PlayOptions options;
    options.report = report.string();
    options.trace  = true;

    grab::cli::RunTrace trace;
    EXPECT_EQ( grab::cli::play_program( program, runner, options, &trace ),
               successExit );

    std::ifstream stream{ report };
    ASSERT_TRUE( stream.good() );
    std::vector<std::string> lines;
    std::string              line;
    while( std::getline( stream, line ) )
    {
        lines.push_back( line );
    }

    ASSERT_EQ( lines.size(), threeLines );
    EXPECT_FALSE( contains( lines[firstStep], traceKindFragment ) ) << lines[firstStep];
    EXPECT_TRUE( contains( lines.back(), traceKindFragment ) ) << lines.back();
    EXPECT_TRUE( contains( lines.back(), schedulingKeyFragment ) ) << lines.back();
    EXPECT_TRUE( contains( lines.back(), runTalliesKeyFragment ) ) << lines.back();
}

// ── --trail and --feedback ────────────────────────────────
//
// The user need these exist for: "I am playing back a json script and I want to
// enable feedback overlays + trail of the mouse." That took three processes --
// `grab trail &`, `grab feedback &`, then `grab play` -- each opening its own
// Session against the same display, started in order and killed afterwards.

TEST( PlayCommand,
      TheVisualFlagsAreOffByDefault )
{
    const std::array values{ documentPath };
    const auto       options = grab::cli::parse_play_options( arguments( values ) );

    ASSERT_TRUE( options.has_value() ) << options.error().message;
    EXPECT_FALSE( options->trail );
    EXPECT_FALSE( options->feedback );
}

// THE DEFAULT THAT DECIDES WHETHER THE FEATURE LOOKS BROKEN. `grab trail`
// distinguishes physical input from XTest-injected input, and under `grab play`
// EVERY sample is injected -- so the trail is drawn entirely in
// `injected_color`. Both colours default to the same amber, which is what makes
// a bare `--trail` visible; a dim or distinct injected default would render a
// working feature indistinguishable from a broken one.
TEST( PlayCommand,
      TheTrailDefaultsMatchTheStandaloneVerbAndAreVisibleWhenInjected )
{
    const std::array values{ documentPath, trailFlag };
    const auto       options = grab::cli::parse_play_options( arguments( values ) );

    ASSERT_TRUE( options.has_value() ) << options.error().message;
    EXPECT_TRUE( options->trail );
    EXPECT_TRUE( same_color( options->trail_style.physical_color,
                             grab::overlay::defaultOverlayColor ) );
    EXPECT_TRUE( same_color( options->trail_style.injected_color,
                             grab::overlay::defaultOverlayColor ) );
    EXPECT_TRUE( same_color( options->trail_style.injected_color,
                             options->trail_style.physical_color ) );

    // Not zero, which is what `OverlayTrailOptions{}` alone would leave behind:
    // a zero-width, zero-fade trail draws nothing at all.
    EXPECT_EQ( options->trail_style.fade, grab::kernel::presentation::defaultTrailFade );
    EXPECT_EQ( options->trail_style.width_px,
               grab::kernel::presentation::defaultTrailWidthPx );
}

TEST( PlayCommand,
      TheFeedbackDefaultsMatchTheStandaloneVerb )
{
    const std::array values{ documentPath, feedbackFlag };
    const auto       options = grab::cli::parse_play_options( arguments( values ) );

    ASSERT_TRUE( options.has_value() ) << options.error().message;
    EXPECT_TRUE( options->feedback );

    // Both presenters on. `CursorFeedbackConfig{}` leaves both nullopt, which
    // is the configuration that draws nothing.
    ASSERT_TRUE( options->feedback_style.click.has_value() );
    ASSERT_TRUE( options->feedback_style.hold.has_value() );
    EXPECT_EQ( options->feedback_style.click->radius_px, grab::RippleStyle{}.radius_px );
    EXPECT_EQ( options->feedback_style.hold->width_px, grab::ProgressStyle{}.width_px );
    EXPECT_EQ( options->feedback_style.thresholds.hold, grab::GestureThresholds{}.hold );
}

TEST( PlayCommand,
      ParsesEveryTrailStyleFlag )
{
    const std::array values{
        documentPath,
        trailFlag,
        trailColorFlag,
        trailColorValue,
        injectedColorFlag,
        injectedColorValue,
        fadeMsFlag,
        fadeValue,
        trailWidthFlag,
        widthValue
    };
    const auto options = grab::cli::parse_play_options( arguments( values ) );

    ASSERT_TRUE( options.has_value() ) << options.error().message;
    EXPECT_TRUE( options->trail );
    EXPECT_TRUE( same_color( options->trail_style.physical_color,
                             grab::overlay::Color{
                                 .r = noChannel,
                                 .g = fullChannel,
                                 .b = noChannel,
                                 .a = fullChannel,
                             } ) );
    EXPECT_TRUE( same_color( options->trail_style.injected_color,
                             grab::overlay::Color{
                                 .r = redOfInjected,
                                 .g = greenOfInjected,
                                 .b = blueOfInjected,
                                 .a = fullChannel,
                             } ) );
    EXPECT_EQ( options->trail_style.fade, trailFade );
    EXPECT_EQ( options->trail_style.width_px, trailWidth );
}

TEST( PlayCommand,
      ParsesEveryFeedbackStyleFlag )
{
    const std::array values{
        documentPath,
        feedbackFlag,
        holdMsFlag,
        holdValue,
        rippleRadiusFlag,
        rippleRadiusValue
    };
    const auto options = grab::cli::parse_play_options( arguments( values ) );

    ASSERT_TRUE( options.has_value() ) << options.error().message;
    EXPECT_TRUE( options->feedback );
    EXPECT_EQ( options->feedback_style.thresholds.hold, holdThreshold );
    ASSERT_TRUE( options->feedback_style.click.has_value() );
    EXPECT_EQ( options->feedback_style.click->radius_px, rippleRadius );
}

TEST( PlayCommand,
      NoClickAndNoHoldSuppressTheirPresenters )
{
    const std::array values{ documentPath, feedbackFlag, noClickFlag, noHoldFlag };
    const auto       options = grab::cli::parse_play_options( arguments( values ) );

    ASSERT_TRUE( options.has_value() ) << options.error().message;
    EXPECT_FALSE( options->feedback_style.click.has_value() );
    EXPECT_FALSE( options->feedback_style.hold.has_value() );
}

// --no-click wins wherever it appears on the line, so the ripple radius set
// before it does not resurrect a presenter the caller turned off.
TEST( PlayCommand,
      NoClickWinsOverARadiusSetEarlierOnTheLine )
{
    const std::array values{
        documentPath,
        feedbackFlag,
        rippleRadiusFlag,
        rippleRadiusValue,
        noClickFlag
    };
    const auto options = grab::cli::parse_play_options( arguments( values ) );

    ASSERT_TRUE( options.has_value() ) << options.error().message;
    EXPECT_FALSE( options->feedback_style.click.has_value() );
}

// A style flag without its feature is an ERROR naming the missing flag. The
// failure being prevented is `--fade-ms 400` on its own: it would otherwise
// parse, run, draw nothing, and leave no evidence but an absent trail.
TEST( PlayCommand,
      RejectsATrailStyleWithoutTheTrailFlag )
{
    const std::array values{ documentPath, fadeMsFlag, fadeValue };
    const auto       options = grab::cli::parse_play_options( arguments( values ) );

    ASSERT_FALSE( options.has_value() );
    EXPECT_TRUE( contains( options.error().message, fadeMsFlag ) )
        << options.error().message;
    EXPECT_TRUE( contains( options.error().message, trailFlag ) )
        << options.error().message;
}

TEST( PlayCommand,
      RejectsAFeedbackStyleWithoutTheFeedbackFlag )
{
    const std::array values{ documentPath, rippleRadiusFlag, rippleRadiusValue };
    const auto       options = grab::cli::parse_play_options( arguments( values ) );

    ASSERT_FALSE( options.has_value() );
    EXPECT_TRUE( contains( options.error().message, rippleRadiusFlag ) )
        << options.error().message;
    EXPECT_TRUE( contains( options.error().message, feedbackFlag ) )
        << options.error().message;
}

TEST( PlayCommand,
      RejectsABooleanFeedbackStyleWithoutTheFeedbackFlag )
{
    const std::array values{ documentPath, noHoldFlag };

    EXPECT_FALSE( grab::cli::parse_play_options( arguments( values ) ).has_value() );
}

// The check is made after the whole line is read, so the two orders agree.
// Deciding at the point of the flag would make one an error and the other a
// success, which is a rule nobody can guess.
TEST( PlayCommand,
      AStyleFlagMayPrecedeItsFeatureFlag )
{
    const std::array values{ documentPath, fadeMsFlag, fadeValue, trailFlag };
    const auto       options = grab::cli::parse_play_options( arguments( values ) );

    ASSERT_TRUE( options.has_value() ) << options.error().message;
    EXPECT_EQ( options->trail_style.fade, trailFade );
}

TEST( PlayCommand,
      RejectsAMalformedTrailColor )
{
    const std::array values{ documentPath, trailFlag, trailColorFlag, malformedColor };

    EXPECT_FALSE( grab::cli::parse_play_options( arguments( values ) ).has_value() );
}

TEST( PlayCommand,
      RejectsAStyleFlagWithNoValue )
{
    const std::array values{ documentPath, trailFlag, fadeMsFlag };

    EXPECT_FALSE( grab::cli::parse_play_options( arguments( values ) ).has_value() );
}

TEST( PlayCommand,
      RejectsANonNumericTrailWidth )
{
    const std::array values{ documentPath, trailFlag, trailWidthFlag, notANumber };

    EXPECT_FALSE( grab::cli::parse_play_options( arguments( values ) ).has_value() );
}

// The trail is assembled from the drive loop, not from a second thread: the
// hook is what carries the observation queue into the animator while the run
// proceeds. A run that never calls it draws nothing however well the rest is
// wired, so the wiring is asserted display-free here.
TEST( PlayCommand,
      TheDriveLoopRunsThePumpHook )
{
    const auto program = wait_after_click(
        PacingOptions{ .mode = PacingMode::Strict, .grace = documentGrace }
    );
    ScriptedRunner               runner{ Status::Success };
    const grab::cli::PlayOptions options;

    std::size_t                  pumps = 0U;
    EXPECT_EQ( grab::cli::play_program( program,
                                        runner,
                                        options,
                                        nullptr,
                                        [&pumps]
                                        {
                                            ++pumps;
                                        } ),
               successExit );
    EXPECT_GE( pumps, onePump );
}

TEST( PlayCommand,
      TheDriveLoopRunsWithoutAPumpHook )
{
    const auto program = wait_after_click(
        PacingOptions{ .mode = PacingMode::Strict, .grace = documentGrace }
    );
    ScriptedRunner               runner{ Status::Success };
    const grab::cli::PlayOptions options;

    EXPECT_EQ( grab::cli::play_program( program, runner, options ), successExit );
}
