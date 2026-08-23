// The public sequence API (grab/sequence.hpp): the loader facade over the
// interpreter, and play() against a live Session.
//
// The load half is display-free. The play half opens a real Session and a
// real input seat, so those tests run under the ctest Xvfb fixture
// (DISPLAY=:87) like every other X-backed suite in this binary.

#include "grab/result.hpp"
#include "grab/sequence.hpp"
#include "grab/sequence_types.hpp"
#include "grab/session.hpp"
#include "grab/trace.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
// clang-format on

namespace
{

    using grab::sequence::PacingMode;
    using grab::sequence::PlayState;
    using grab::sequence::StepStatus;

    constexpr std::string_view labelledDocument = R"({
        "sequence": "api-flow",
        "pacing": { "mode": "grace", "grace_ms": 5 },
        "steps": [
            { "id": "warp", "op": "input.warp", "to": [10, 10] },
            { "id": "settle", "op": "time.wait", "ms": 25 },
            { "op": "input.click" }
        ]
    })";

    [[nodiscard]]
    std::filesystem::path
    corpus_file( std::string_view relative )
    {
        return std::filesystem::path{ GRAB_SEQUENCE_CORPUS_DIR } / relative;
    }

    TEST( SequenceApi,
          LoadParsesAndValidatesADocument )
    {
        const auto loaded = grab::sequence::load( labelledDocument );
        ASSERT_TRUE( loaded.has_value() ) << loaded.error().message;

        EXPECT_EQ( loaded->name(), "api-flow" );
        EXPECT_EQ( loaded->step_count(), 3U );
        EXPECT_EQ( loaded->pacing().mode, PacingMode::Grace );
        EXPECT_EQ( loaded->pacing().grace, std::chrono::milliseconds{ 5 } );

        const auto warp = loaded->resolve_label( "warp" );
        ASSERT_TRUE( warp.has_value() );
        EXPECT_EQ( loaded->label_of( *warp ), "warp" );
        EXPECT_EQ( loaded->op_of( *warp ), "input.warp" );

        // The unlabelled step is still addressable positionally: identity is
        // assigned by document position, never by label.
        const grab::sequence::StepId third{
            2U,
            grab::sequence::StepId::firstGeneration,
        };
        EXPECT_EQ( loaded->label_of( third ), "" );
        EXPECT_EQ( loaded->op_of( third ), "input.click" );

        EXPECT_FALSE( loaded->resolve_label( "absent" ).has_value() );
    }

    TEST( SequenceApi,
          LoadFileReadsTheCorpus )
    {
        const auto loaded =
            grab::sequence::load_file( corpus_file( "valid/spec-example.json" ) );
        ASSERT_TRUE( loaded.has_value() ) << loaded.error().message;
        EXPECT_GT( loaded->step_count(), 0U );
    }

    // The diagnostics ARE the contract: an error names the offending step and
    // field with the loader's JSON pointer, not merely "invalid sequence".
    TEST( SequenceApi,
          LoadRejectsADanglingAfterWithAPointer )
    {
        const auto loaded = grab::sequence::load(
            R"({ "steps": [ { "op": "input.click", "after": ["missing"] } ] })"
        );
        ASSERT_FALSE( loaded.has_value() );
        EXPECT_NE( loaded.error().message.find( "/steps/0" ), std::string::npos )
            << loaded.error().message;
    }

    TEST( SequenceApi,
          LoadRejectsAnUnknownOpByName )
    {
        const auto loaded =
            grab::sequence::load( R"({ "steps": [ { "op": "input.zap" } ] })" );
        ASSERT_FALSE( loaded.has_value() );
        EXPECT_NE( loaded.error().message.find( "input.zap" ), std::string::npos )
            << loaded.error().message;
    }

    TEST( SequenceApi,
          LoadFileNamesTheFileInItsErrors )
    {
        const auto loaded =
            grab::sequence::load_file( corpus_file( "invalid/dangling-after.json" ) );
        ASSERT_FALSE( loaded.has_value() );
        EXPECT_NE(
            loaded.error().message.find( "dangling-after.json" ),
            std::string::npos
        ) << loaded.error().message;
    }

    TEST( SequenceApi,
          ToJsonRoundTrips )
    {
        const auto loaded = grab::sequence::load( labelledDocument );
        ASSERT_TRUE( loaded.has_value() ) << loaded.error().message;

        const auto json = grab::sequence::to_json( *loaded );
        ASSERT_TRUE( json.has_value() ) << json.error().message;

        const auto again = grab::sequence::load( *json );
        ASSERT_TRUE( again.has_value() ) << again.error().message;
        EXPECT_EQ( again->step_count(), loaded->step_count() );
        EXPECT_EQ( again->name(), loaded->name() );
        ASSERT_TRUE( again->resolve_label( "settle" ).has_value() );
        EXPECT_EQ( *again->resolve_label( "settle" ),
                   *loaded->resolve_label( "settle" ) );
    }

    TEST( SequenceApi,
          SessionAnswersItsDisplayOption )
    {
        grab::SessionOptions options;
        options.display = ":87";
        auto session    = grab::Session::open( options );
        ASSERT_TRUE( session.has_value() ) << session.error().message;
        ASSERT_TRUE( ( *session )->display().has_value() );
        EXPECT_EQ( *( *session )->display(), ":87" );
    }

    TEST( SequenceApi,
          PlayRefusesAClosedSession )
    {
        auto session = grab::Session::open();
        ASSERT_TRUE( session.has_value() ) << session.error().message;
        ( *session )->close();

        const auto loaded = grab::sequence::load( labelledDocument );
        ASSERT_TRUE( loaded.has_value() );

        const auto report = grab::sequence::play( **session, *loaded );
        ASSERT_FALSE( report.has_value() );
        EXPECT_EQ( report.error().code, grab::ErrorCode::SessionClosed );
    }

    TEST( SequenceApi,
          PlayRunsADocumentToDone )
    {
        auto session = grab::Session::open();
        ASSERT_TRUE( session.has_value() ) << session.error().message;

        const auto loaded = grab::sequence::load( labelledDocument );
        ASSERT_TRUE( loaded.has_value() ) << loaded.error().message;

        // Overriding the document's grace pacing back to strict exercises the
        // rebuild path; ids are positional, so the outcomes below still line
        // up with the document.
        grab::sequence::PlayOptions options;
        options.mode      = PacingMode::Strict;

        const auto report = grab::sequence::play( **session, *loaded, options );
        ASSERT_TRUE( report.has_value() ) << report.error().message;
        EXPECT_TRUE( report->succeeded() );
        EXPECT_EQ( report->state, PlayState::Done );
        EXPECT_FALSE( report->failure.has_value() );
        ASSERT_EQ( report->steps.size(), 3U );

        for( std::size_t index = 0U; index < report->steps.size(); ++index )
        {
            const auto& step = report->steps.at( index );
            EXPECT_EQ( step.id.index(), index );
            EXPECT_EQ( step.status, StepStatus::Succeeded );
        }
        EXPECT_EQ( report->steps.at( 0U ).op, "input.warp" );
        EXPECT_EQ( report->steps.at( 1U ).label, "settle" );

        // time.wait is the one op whose duration the document declares, and
        // the run cannot legally finish before it elapsed.
        ASSERT_TRUE( report->steps.at( 1U ).declared.has_value() );
        EXPECT_EQ( *report->steps.at( 1U ).declared, std::chrono::milliseconds{ 25 } );
        EXPECT_GE( report->elapsed, std::chrono::milliseconds{ 25 } );
        EXPECT_FALSE( report->run_id.is_nil() );
    }

    // A step failing is reported IN the PlayReport, keyed by StepId — the
    // caller learns WHICH step failed, not merely that one did.
    TEST( SequenceApi,
          PlayReportsTheFailingStep )
    {
        auto session = grab::Session::open();
        ASSERT_TRUE( session.has_value() ) << session.error().message;

        // screen.capture by locator is legal grammar with no destination the
        // seat can serve, so step 1 fails at enter and the abort policy
        // unwinds the run before step 2.
        const auto loaded = grab::sequence::load( R"({
            "steps": [
                { "op": "input.warp", "to": [5, 5] },
                { "id": "shot", "op": "screen.capture", "locator": "role:frame" },
                { "op": "input.click" }
            ]
        })" );
        ASSERT_TRUE( loaded.has_value() ) << loaded.error().message;

        const auto report = grab::sequence::play( **session, *loaded );
        ASSERT_TRUE( report.has_value() ) << report.error().message;
        EXPECT_FALSE( report->succeeded() );
        EXPECT_EQ( report->state, PlayState::Interrupted );
        ASSERT_TRUE( report->failure.has_value() );
        ASSERT_EQ( report->steps.size(), 3U );
        EXPECT_EQ( report->steps.at( 0U ).status, StepStatus::Succeeded );
        EXPECT_EQ( report->steps.at( 1U ).status, StepStatus::Failed );
        EXPECT_NE( report->steps.at( 2U ).status, StepStatus::Succeeded );
    }

    TEST( SequenceApi,
          PlayHonoursTheStopToken )
    {
        auto session = grab::Session::open();
        ASSERT_TRUE( session.has_value() ) << session.error().message;

        // Five seconds of wait, cancelled before the first pump: the run must
        // come back unwound in far less than its own document would take.
        const auto loaded = grab::sequence::load(
            R"({ "steps": [ { "op": "time.wait", "ms": 5000 } ] })"
        );
        ASSERT_TRUE( loaded.has_value() ) << loaded.error().message;

        std::stop_source stop;
        stop.request_stop();
        grab::sequence::PlayOptions options;
        options.stop       = stop.get_token();

        const auto started = std::chrono::steady_clock::now();
        const auto report  = grab::sequence::play( **session, *loaded, options );
        const auto took    = std::chrono::steady_clock::now() - started;

        ASSERT_TRUE( report.has_value() ) << report.error().message;
        EXPECT_FALSE( report->succeeded() );
        EXPECT_EQ( report->state, PlayState::Interrupted );
        ASSERT_TRUE( report->failure.has_value() );
        EXPECT_EQ( report->failure->code, grab::ErrorCode::Cancelled );
        EXPECT_LT( took, std::chrono::seconds{ 4 } );
    }

    // The borrowed-session path: overlay steps land on the CALLER's session
    // surface rather than on a second session the seat opens for itself. The
    // overlay needs a compositing manager, and the ctest fixture runs one on
    // :88 — so unlike the input-only tests this one names its display, and
    // play() must follow the session there for input too.
    TEST( SequenceApi,
          PlayDrawsOverlayStepsOnTheCallersSession )
    {
        grab::SessionOptions composited;
        composited.display = ":88";
        auto session       = grab::Session::open( composited );
        ASSERT_TRUE( session.has_value() ) << session.error().message;

        const auto loaded = grab::sequence::load( R"({
            "steps": [
                { "id": "add", "op": "overlay.add", "handle": "c01",
                  "shape": { "rect": { "x": 4, "y": 4, "w": 24, "h": 24 },
                             "stroke": { "color": "#ffaa00", "width": 2 } } },
                { "op": "overlay.remove", "handle": "c01" }
            ]
        })" );
        ASSERT_TRUE( loaded.has_value() ) << loaded.error().message;

        const auto report = grab::sequence::play( **session, *loaded );
        ASSERT_TRUE( report.has_value() ) << report.error().message;
        EXPECT_TRUE( report->succeeded() )
            << ( report->failure.has_value() ? report->failure->message
                                             : std::string{} );
        ASSERT_EQ( report->steps.size(), 2U );
        EXPECT_EQ( report->steps.at( 0U ).status, StepStatus::Succeeded );
        EXPECT_EQ( report->steps.at( 1U ).status, StepStatus::Succeeded );
    }

}    // namespace
