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
#include "grab/result.hpp"
#include "grab/sequence_types.hpp"
#include "grab/trace.hpp"
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

    constexpr std::string_view          playVerb            = "play";
    constexpr std::string_view          sequenceLine        = "sequence: test-flow";
    constexpr std::string_view          stepsLine           = "steps: 2";
    constexpr std::string_view          orderLine           = "order: 0 1";
    constexpr std::string_view          planPrefix          = "plan: >= ";
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

            void
            refuse_text() noexcept
            {
                refuse_text_ = true;
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
            bool                               refuse_text_{ false };
    };

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
// input.press was exited by succeed(). Its hold therefore survives the abort,
// which is exactly what release_outstanding() is for.
TEST( PlayCommand,
      AnAbortAfterASuccessfulPressStillReleasesTheButton )
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
    EXPECT_EQ( runner.outstanding_holds(), oneOutstandingHold );

    EXPECT_EQ( runner.release_outstanding(), grab::NeutralizationOutcome::Released );
    ASSERT_EQ( seat.buttons().size(), twoSteps );
    EXPECT_FALSE( seat.buttons()[1U].pressed );
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
