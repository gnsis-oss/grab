// The sequence loader: JSON bytes in, a typed Sequence out, and back again.
//
// The properties worth restating, because they are the ones easiest to break:
//
//   * A step with no `after` depends on the PRECEDING step in document order,
//     so a plain list reads top-to-bottom like the bash script it replaces.
//   * `id` is an author label, NEVER the identity. Every step gets a positional
//     StepId whether or not it carries one, which is why two byte-identical
//     unlabelled clicks are still different steps.
//   * Positional identity is also what makes parse -> to_json -> parse come
//     back identical WITHOUT ids ever being written into the document.
//   * `extra_grace_ms` under `strict` LOADS and is ignored. Rejecting it would
//     stop one document running under all three pacing modes, which is the
//     entire point of having modes.

#include "grab/command.hpp"
#include "grab/command_descriptor.hpp"
#include "grab/result.hpp"
#include "grab/sequence_types.hpp"
#include "kernel/sequence/interpreter.hpp"
#include "kernel/sequence/sequence.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>
// clang-format on

namespace
{

    using grab::kernel::sequence::load;
    using grab::kernel::sequence::parse;
    using grab::kernel::sequence::Sequence;
    using grab::kernel::sequence::to_json;

    // ── Positions ────────────────────────────────────────────

    using Half                                 = grab::sequence::StepId::Half;

    constexpr Half             firstIndex      = 0U;
    constexpr Half             secondIndex     = 1U;
    constexpr Half             thirdIndex      = 2U;
    constexpr Half             fourthIndex     = 3U;
    constexpr Half             fifthIndex      = 4U;
    constexpr Half             fifteenthIndex  = 14U;

    constexpr std::size_t      noSteps         = 0U;
    constexpr std::size_t      oneEdge         = 1U;
    constexpr std::size_t      twoEdges        = 2U;
    constexpr std::size_t      twoSteps        = 2U;
    constexpr std::size_t      threeSteps      = 3U;
    constexpr std::size_t      fiveSteps       = 5U;
    constexpr std::size_t      graphFaultCount = 5U;

    // ── Payload values the assertions expect ─────────────────

    constexpr std::int32_t     warpX                     = 10;
    constexpr std::int32_t     warpY                     = 20;
    constexpr std::int64_t     waitMilliseconds          = 250;
    constexpr std::int64_t     extraGraceMilliseconds    = 400;
    constexpr std::int64_t     documentGraceMilliseconds = 80;
    constexpr std::uint8_t     leftButton                = 1U;
    constexpr std::uint8_t     middleButton              = 2U;
    constexpr std::uint8_t     rightButton               = 3U;
    constexpr std::uint8_t     wheelDownButton           = 5U;

    // ── Message fragments ────────────────────────────────────

    constexpr std::string_view unknownOpName     = "input.frobnicate";
    constexpr std::string_view unknownOpPhrase   = "unknown op";
    constexpr std::string_view unavailablePhrase = "is not available as a sequence step";
    constexpr std::string_view danglingPhrase    = "no step carries that label";
    constexpr std::string_view duplicatePhrase   = "duplicate step label";
    constexpr std::string_view cyclePhrase       = "cycle";
    constexpr std::string_view selfEdgePhrase    = "depends on itself";
    constexpr std::string_view repeatedPhrase    = "twice";
    constexpr std::string_view missingFilePhrase = "file not found";
    constexpr std::string_view unknownModePhrase = "unknown pacing mode";
    constexpr std::string_view opPointer         = "/steps/3/op";
    constexpr std::string_view danglingPointer   = "/steps/1/after/0";
    constexpr std::string_view duplicatePointer  = "/steps/1/id";
    constexpr std::string_view selfEdgePointer   = "/steps/0/after/0";
    constexpr std::string_view repeatedPointer   = "/steps/1/after/1";
    constexpr std::string_view gracePointer      = "/pacing/grace_ms";
    constexpr std::string_view extraGracePointer = "/steps/1/extra_grace_ms";
    constexpr std::string_view badStepLabel      = "step 'bad'";
    constexpr std::string_view recoverLabel      = "recover";

    constexpr std::string_view builtSequenceName = "built-by-hand";
    constexpr std::string_view thirdLabel        = "third";

    constexpr std::string_view sequenceFileName  = "grab-interpreter-unit07.json";
    constexpr std::string_view badFileName       = "grab-interpreter-unit07-bad.json";
    constexpr std::string_view missingFileName   = "grab-interpreter-unit07-absent.json";

    // ── Documents ────────────────────────────────────────────

    // The spec's own example, verbatim from the design's section 3.
    constexpr std::string_view specExample           = R"({
  "schema_version": 1,
  "sequence": "login-flow",
  "pacing": { "mode": "grace", "grace_ms": 80 },
  "steps": [
    { "id": "move",  "op": "input.move",     "to": [640, 400] },
    { "id": "wait",  "op": "time.wait",      "ms": 250 },
    { "id": "click", "op": "input.click",    "button": "left" },
    { "id": "shot",  "op": "screen.capture", "out": "a.png", "after": ["click"] },
    { "id": "type",  "op": "input.type",     "text": "hi",   "after": ["click"],
      "extra_grace_ms": 400 }
  ]
})";

    constexpr std::string_view threeImplicitSteps    = R"({
  "steps": [
    { "op": "input.warp", "to": [10, 20] },
    { "op": "time.wait",  "ms": 250 },
    { "op": "input.click" }
  ]
})";

    constexpr std::string_view explicitAfterDocument = R"({
  "steps": [
    { "id": "root", "op": "input.click" },
    { "op": "time.wait", "ms": 250 },
    { "op": "input.type", "text": "hi", "after": ["root"] }
  ]
})";

    constexpr std::string_view forkDocument          = R"({
  "steps": [
    { "id": "click", "op": "input.click" },
    { "op": "screen.capture", "out": "a.png", "after": ["click"] },
    { "op": "input.type", "text": "hi", "after": ["click"] }
  ]
})";

    constexpr std::string_view unknownOpDocument     = R"({
  "steps": [
    { "op": "input.click" },
    { "op": "time.wait", "ms": 250 },
    { "op": "input.click" },
    { "id": "bad", "op": "input.frobnicate" }
  ]
})";

    // Every one of these resolves through command_kind() and has no payload
    // struct, so it is a different author mistake from a misspelling.
    constexpr std::string_view doctorDocument          = R"({
  "steps": [
    { "op": "input.click" },
    { "op": "time.wait", "ms": 250 },
    { "op": "input.click" },
    { "id": "bad", "op": "system.doctor" }
  ]
})";

    constexpr std::string_view dragCurveDocument       = R"({
  "steps": [
    { "op": "input.click" },
    { "op": "time.wait", "ms": 250 },
    { "op": "input.click" },
    { "id": "bad", "op": "input.drag_curve" }
  ]
})";

    constexpr std::string_view overlayTrailDocument    = R"({
  "steps": [
    { "op": "input.click" },
    { "op": "time.wait", "ms": 250 },
    { "op": "input.click" },
    { "id": "bad", "op": "overlay.trail" }
  ]
})";

    constexpr std::string_view sessionDocument         = R"({
  "steps": [
    { "op": "input.click" },
    { "op": "time.wait", "ms": 250 },
    { "op": "input.click" },
    { "id": "bad", "op": "session.open" }
  ]
})";

    constexpr std::string_view playDocument            = R"({
  "steps": [
    { "op": "input.click" },
    { "op": "time.wait", "ms": 250 },
    { "op": "input.click" },
    { "id": "bad", "op": "system.play" }
  ]
})";

    constexpr std::string_view danglingAfterDocument   = R"({
  "steps": [
    { "id": "here", "op": "input.click" },
    { "id": "there", "op": "input.click", "after": ["nowhere"] }
  ]
})";

    constexpr std::string_view duplicateLabelDocument  = R"({
  "steps": [
    { "id": "same", "op": "input.click" },
    { "id": "same", "op": "input.click" }
  ]
})";

    constexpr std::string_view cycleDocument           = R"({
  "steps": [
    { "id": "a", "op": "input.click", "after": ["c"] },
    { "id": "b", "op": "input.click", "after": ["a"] },
    { "id": "c", "op": "input.click", "after": ["b"] }
  ]
})";

    constexpr std::string_view selfEdgeDocument        = R"({
  "steps": [
    { "id": "loop", "op": "input.click", "after": ["loop"] }
  ]
})";

    constexpr std::string_view repeatedAfterDocument   = R"({
  "steps": [
    { "id": "once", "op": "input.click" },
    { "id": "twice", "op": "input.click", "after": ["once", "once"] }
  ]
})";

    constexpr std::string_view identicalClicksDocument = R"({
  "steps": [
    { "op": "input.click", "button": "left" },
    { "op": "input.click", "button": "left" }
  ]
})";

    constexpr std::string_view strictWithExtraGrace    = R"({
  "pacing": { "mode": "strict" },
  "steps": [
    { "id": "a", "op": "input.click" },
    { "id": "b", "op": "input.type", "text": "hi", "extra_grace_ms": 400 }
  ]
})";

    constexpr std::string_view preciseWithExtraGrace   = R"({
  "pacing": { "mode": "precise", "grace_ms": 80 },
  "steps": [
    { "id": "a", "op": "input.click" },
    { "id": "b", "op": "input.type", "text": "hi", "extra_grace_ms": 400 }
  ]
})";

    constexpr std::string_view emptyStepsDocument      = R"({ "steps": [] })";

    constexpr std::string_view missingStepsDocument    = R"({ "sequence": "nothing" })";

    constexpr std::string_view unknownModeDocument     = R"({
  "pacing": { "mode": "eventually" },
  "steps": [ { "op": "input.click" } ]
})";

    constexpr std::string_view negativeGraceDocument   = R"({
  "pacing": { "mode": "grace", "grace_ms": -1 },
  "steps": [ { "op": "input.click" } ]
})";

    constexpr std::string_view negativeExtraGraceDocument = R"({
  "steps": [
    { "op": "input.click" },
    { "op": "input.click", "extra_grace_ms": -1 }
  ]
})";

    constexpr std::string_view waitWithoutDuration        = R"({
  "steps": [ { "id": "w", "op": "time.wait" } ]
})";

    constexpr std::string_view captureWithBothTargets     = R"({
  "steps": [
    { "id": "shot", "op": "screen.capture", "out": "a.png", "locator": "window:1" }
  ]
})";

    constexpr std::string_view explicitRootDocument       = R"({
  "steps": [
    { "id": "a", "op": "input.click" },
    { "id": "b", "op": "input.click", "after": [] }
  ]
})";

    constexpr std::string_view captureByLocatorDocument   = R"({
  "steps": [
    { "id": "shot", "op": "screen.capture", "locator": "window:title=Firefox" }
  ]
})";

    constexpr std::string_view gotoDocument               = R"({
  "steps": [
    { "id": "recover", "op": "input.click" },
    { "id": "risky", "op": "screen.capture", "out": "a.png",
      "on_error": "goto:recover" }
  ]
})";

    constexpr std::string_view gotoNowhereDocument        = R"({
  "steps": [
    { "id": "recover", "op": "input.click" },
    { "id": "risky", "op": "screen.capture", "out": "a.png",
      "on_error": "goto:elsewhere" }
  ]
})";

    constexpr std::string_view buttonSpellingsDocument    = R"({
  "steps": [
    { "op": "input.click", "button": "middle" },
    { "op": "input.press", "button": 3 },
    { "op": "input.release", "button": "wheel_down" }
  ]
})";

    // One step per sequence-capable command: 15 of the descriptor table's 30.
    constexpr std::string_view everyOpDocument = R"({
  "schema_version": 1,
  "sequence": "every-op",
  "pacing": { "mode": "precise", "grace_ms": 25 },
  "steps": [
    { "id": "warp",    "op": "input.warp",     "to": [10, 20] },
    { "id": "move",    "op": "input.move",     "from": [10, 20], "to": [30, 40],
      "options": { "steps": 8, "step_dwell_ms": 4, "path": "cubic" } },
    { "id": "follow",  "op": "input.follow",
      "curve": [[0.0, 0.0], [10.5, 20.25], [30.0, 40.0]] },
    { "id": "press",   "op": "input.press",    "button": "middle" },
    { "id": "release", "op": "input.release",  "button": "middle" },
    { "id": "click",   "op": "input.click",    "button": "left" },
    { "id": "clickat", "op": "input.click_at", "at": [50, 60], "button": "right" },
    { "id": "drag",    "op": "input.drag",     "from": [1, 2], "to": [3, 4],
      "button": 1 },
    { "id": "scroll",  "op": "input.scroll",   "dx": -1, "dy": 3 },
    { "id": "keydown", "op": "input.key_down", "key": "Control_L" },
    { "id": "key",     "op": "input.key",      "key": "c" },
    { "id": "keyup",   "op": "input.key_up",   "key": "Control_L" },
    { "id": "type",    "op": "input.type",     "text": "héllo ⌘",
      "extra_grace_ms": 400 },
    { "id": "wait",    "op": "time.wait",      "ms": 250, "on_error": "continue" },
    { "id": "shot",    "op": "screen.capture", "out": "a.png",
      "after": ["click", "type"], "on_error": "goto:warp" }
  ]
})";

    // ── Helpers ──────────────────────────────────────────────

    [[nodiscard]]
    constexpr grab::sequence::StepId
    step_id( Half index ) noexcept
    {
        return grab::sequence::StepId{ index, grab::sequence::StepId::firstGeneration };
    }

    [[nodiscard]]
    bool
    mentions( const std::string& message,
              std::string_view   fragment )
    {
        return message.find( fragment ) != std::string::npos;
    }

    [[nodiscard]]
    std::string
    rejection_of( std::string_view document )
    {
        const auto parsed = parse( document );
        if( parsed.has_value() )
        {
            return {};
        }
        return parsed.error().message;
    }

    // Identity in the sense the round-trip claims: same ids, same edges, same
    // labels, same policies, same order, and a byte-identical serialization.
    void
    expect_identical( const Sequence& lhs,
                      const Sequence& rhs )
    {
        EXPECT_EQ( lhs.name(), rhs.name() );
        EXPECT_EQ( lhs.pacing().mode, rhs.pacing().mode );
        EXPECT_EQ( lhs.pacing().grace, rhs.pacing().grace );

        ASSERT_EQ( lhs.steps().size(), rhs.steps().size() );
        for( std::size_t index = 0U; index < lhs.steps().size(); ++index )
        {
            const auto& leftStep  = lhs.steps()[index];
            const auto& rightStep = rhs.steps()[index];

            EXPECT_EQ( leftStep.id.bits(), rightStep.id.bits() );
            EXPECT_EQ( leftStep.label, rightStep.label );
            EXPECT_EQ( leftStep.on_error, rightStep.on_error );
            EXPECT_EQ( leftStep.on_error_target, rightStep.on_error_target );
            EXPECT_EQ( leftStep.extra_grace, rightStep.extra_grace );
            EXPECT_EQ( grab::sequence::kind_of( leftStep.command ),
                       grab::sequence::kind_of( rightStep.command ) );

            ASSERT_EQ( leftStep.after.size(), rightStep.after.size() );
            for( std::size_t slot = 0U; slot < leftStep.after.size(); ++slot )
            {
                EXPECT_EQ( leftStep.after[slot].bits(), rightStep.after[slot].bits() );
            }
        }

        ASSERT_EQ( lhs.order().size(), rhs.order().size() );
        for( std::size_t index = 0U; index < lhs.order().size(); ++index )
        {
            EXPECT_EQ( lhs.order()[index].bits(), rhs.order()[index].bits() );
        }

        const auto leftText  = to_json( lhs );
        const auto rightText = to_json( rhs );
        ASSERT_TRUE( leftText.has_value() ) << leftText.error().message;
        ASSERT_TRUE( rightText.has_value() ) << rightText.error().message;
        EXPECT_EQ( *leftText, *rightText );
    }

    void
    expect_round_trips( std::string_view document )
    {
        const auto first = parse( document );
        ASSERT_TRUE( first.has_value() ) << first.error().message;
        const auto text = to_json( *first );
        ASSERT_TRUE( text.has_value() ) << text.error().message;
        const auto second = parse( *text );
        ASSERT_TRUE( second.has_value() ) << second.error().message;
        expect_identical( *first, *second );
    }

    [[nodiscard]]
    std::filesystem::path
    scratch_file( std::string_view name )
    {
        return std::filesystem::path{ ::testing::TempDir() } / name;
    }

    // ── Implicit and explicit dependencies ───────────────────

    TEST( Interpreter,
          AStepWithoutAfterDependsOnThePrecedingStep )
    {
        const auto parsed = parse( threeImplicitSteps );
        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;

        const auto steps = parsed->steps();
        ASSERT_EQ( steps.size(), threeSteps );

        // The first step depends on nothing; a plain list then reads
        // top-to-bottom without an `after` on every line.
        EXPECT_TRUE( steps[firstIndex].after.empty() );

        ASSERT_EQ( steps[secondIndex].after.size(), oneEdge );
        EXPECT_EQ( steps[secondIndex].after.front(), step_id( firstIndex ) );

        ASSERT_EQ( steps[thirdIndex].after.size(), oneEdge );
        EXPECT_EQ( steps[thirdIndex].after.front(), step_id( secondIndex ) );
    }

    TEST( Interpreter,
          ExplicitAfterOverridesTheImplicitEdge )
    {
        const auto parsed = parse( explicitAfterDocument );
        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;

        const auto steps = parsed->steps();
        ASSERT_EQ( steps.size(), threeSteps );
        ASSERT_EQ( steps[thirdIndex].after.size(), oneEdge );
        EXPECT_EQ( steps[thirdIndex].after.front(), step_id( firstIndex ) );
    }

    TEST( Interpreter,
          TwoStepsNamingOnePredecessorFork )
    {
        const auto parsed = parse( forkDocument );
        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;

        const auto steps = parsed->steps();
        ASSERT_EQ( steps.size(), threeSteps );
        ASSERT_EQ( steps[secondIndex].after.size(), oneEdge );
        ASSERT_EQ( steps[thirdIndex].after.size(), oneEdge );
        EXPECT_EQ( steps[secondIndex].after.front(), step_id( firstIndex ) );
        EXPECT_EQ( steps[thirdIndex].after.front(), step_id( firstIndex ) );
    }

    TEST( Interpreter,
          AnExplicitlyEmptyAfterDeclaresARootAndSurvivesTheRoundTrip )
    {
        const auto parsed = parse( explicitRootDocument );
        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;
        ASSERT_EQ( parsed->steps().size(), twoSteps );
        EXPECT_TRUE( parsed->steps()[secondIndex].after.empty() );

        expect_round_trips( explicitRootDocument );
    }

    // ── Op resolution ────────────────────────────────────────

    TEST( Interpreter,
          AnUnknownOpNamesTheOpTheStepAndAJsonPointer )
    {
        const auto parsed = parse( unknownOpDocument );
        ASSERT_FALSE( parsed.has_value() );

        const auto& message = parsed.error().message;
        EXPECT_TRUE( mentions( message, unknownOpPhrase ) ) << message;
        EXPECT_TRUE( mentions( message, unknownOpName ) ) << message;
        EXPECT_TRUE( mentions( message, opPointer ) ) << message;
        EXPECT_TRUE( mentions( message, badStepLabel ) ) << message;
    }

    TEST( Interpreter,
          ATableKnownOpWithNoPayloadGetsADifferentMessageFromAnUnknownOp )
    {
        constexpr auto    documents = std::to_array<std::string_view>( {
            doctorDocument,
            dragCurveDocument,
            overlayTrailDocument,
            sessionDocument,
            playDocument,
        } );

        const std::string unknown   = rejection_of( unknownOpDocument );
        ASSERT_FALSE( unknown.empty() );

        for( const auto document : documents )
        {
            const std::string message = rejection_of( document );
            ASSERT_FALSE( message.empty() ) << document;

            // The name is real; the verb simply cannot be a step. That is a
            // different author mistake from a misspelling and must not share
            // its message.
            EXPECT_TRUE( mentions( message, unavailablePhrase ) ) << message;
            EXPECT_FALSE( mentions( message, unknownOpPhrase ) ) << message;
            EXPECT_TRUE( mentions( message, opPointer ) ) << message;
            EXPECT_TRUE( mentions( message, badStepLabel ) ) << message;
            EXPECT_NE( message, unknown );
        }
    }

    TEST( Interpreter,
          EverySequenceCapableOpParses )
    {
        const auto parsed = parse( everyOpDocument );
        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;
        ASSERT_EQ( parsed->steps().size(), grab::sequence::sequenceCommandCount );

        std::vector<grab::CommandKind> kinds;
        kinds.reserve( parsed->steps().size() );
        for( const auto& step : parsed->steps() )
        {
            kinds.push_back( grab::sequence::kind_of( step.command ) );
        }
        std::ranges::sort( kinds );
        EXPECT_EQ( std::ranges::unique( kinds ).begin(), kinds.end() );
    }

    // ── Graph faults ─────────────────────────────────────────

    TEST( Interpreter,
          ADanglingAfterIsRejected )
    {
        const std::string message = rejection_of( danglingAfterDocument );
        ASSERT_FALSE( message.empty() );
        EXPECT_TRUE( mentions( message, danglingPhrase ) ) << message;
        EXPECT_TRUE( mentions( message, danglingPointer ) ) << message;
    }

    TEST( Interpreter,
          ADuplicateLabelIsRejected )
    {
        const std::string message = rejection_of( duplicateLabelDocument );
        ASSERT_FALSE( message.empty() );
        EXPECT_TRUE( mentions( message, duplicatePhrase ) ) << message;
        EXPECT_TRUE( mentions( message, duplicatePointer ) ) << message;
    }

    TEST( Interpreter,
          ACycleIsRejected )
    {
        const std::string message = rejection_of( cycleDocument );
        ASSERT_FALSE( message.empty() );
        EXPECT_TRUE( mentions( message, cyclePhrase ) ) << message;
    }

    TEST( Interpreter,
          ASelfEdgeIsRejected )
    {
        // add_edge drops a self-loop and returns false, so the topological
        // sort never sees one: rejecting it is the loader's job.
        const std::string message = rejection_of( selfEdgeDocument );
        ASSERT_FALSE( message.empty() );
        EXPECT_TRUE( mentions( message, selfEdgePhrase ) ) << message;
        EXPECT_TRUE( mentions( message, selfEdgePointer ) ) << message;
    }

    TEST( Interpreter,
          ARepeatedAfterEntryIsRejected )
    {
        const std::string message = rejection_of( repeatedAfterDocument );
        ASSERT_FALSE( message.empty() );
        EXPECT_TRUE( mentions( message, repeatedPhrase ) ) << message;
        EXPECT_TRUE( mentions( message, repeatedPointer ) ) << message;
    }

    TEST( Interpreter,
          EveryGraphFaultGetsItsOwnMessage )
    {
        const std::array<std::string, graphFaultCount> messages{
            rejection_of( danglingAfterDocument ),
            rejection_of( duplicateLabelDocument ),
            rejection_of( cycleDocument ),
            rejection_of( selfEdgeDocument ),
            rejection_of( repeatedAfterDocument ),
        };

        for( std::size_t left = 0U; left < messages.size(); ++left )
        {
            ASSERT_FALSE( messages[left].empty() ) << left;
            for( std::size_t right = left + 1U; right < messages.size(); ++right )
            {
                EXPECT_NE( messages[left], messages[right] );
            }
        }
    }

    // ── Identity ─────────────────────────────────────────────

    TEST( Interpreter,
          ByteIdenticalUnlabelledClicksGetDifferentStepIds )
    {
        const auto parsed = parse( identicalClicksDocument );
        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;

        const auto steps = parsed->steps();
        ASSERT_EQ( steps.size(), twoSteps );
        EXPECT_TRUE( steps[firstIndex].label.empty() );
        EXPECT_TRUE( steps[secondIndex].label.empty() );

        // Positional, not content-derived: a hash of the payload would collide
        // exactly here, which is the case the format needs distinct.
        EXPECT_NE( steps[firstIndex].id.bits(), steps[secondIndex].id.bits() );
        EXPECT_EQ( steps[firstIndex].id, step_id( firstIndex ) );
        EXPECT_EQ( steps[secondIndex].id, step_id( secondIndex ) );
    }

    // ── Pacing ───────────────────────────────────────────────

    TEST( Interpreter,
          ExtraGraceLoadsUnderStrictAndTheModeStaysStrict )
    {
        const auto parsed = parse( strictWithExtraGrace );
        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;

        EXPECT_EQ( parsed->pacing().mode, grab::sequence::PacingMode::Strict );
        ASSERT_EQ( parsed->steps().size(), twoSteps );
        EXPECT_EQ( parsed->steps()[secondIndex].extra_grace,
                   std::chrono::milliseconds{ extraGraceMilliseconds } );
    }

    TEST( Interpreter,
          TheSameDocumentUnderTwoModesYieldsTheSameStepIds )
    {
        const auto strict  = parse( strictWithExtraGrace );
        const auto precise = parse( preciseWithExtraGrace );
        ASSERT_TRUE( strict.has_value() ) << strict.error().message;
        ASSERT_TRUE( precise.has_value() ) << precise.error().message;

        ASSERT_EQ( strict->steps().size(), precise->steps().size() );
        for( std::size_t index = 0U; index < strict->steps().size(); ++index )
        {
            EXPECT_EQ( strict->steps()[index].id.bits(),
                       precise->steps()[index].id.bits() );
        }
        EXPECT_NE( strict->pacing().mode, precise->pacing().mode );
    }

    TEST( Interpreter,
          AnUnknownPacingModeIsRejected )
    {
        const std::string message = rejection_of( unknownModeDocument );
        ASSERT_FALSE( message.empty() );
        EXPECT_TRUE( mentions( message, unknownModePhrase ) ) << message;
    }

    TEST( Interpreter,
          ANegativeGraceIsRejected )
    {
        const std::string document = rejection_of( negativeGraceDocument );
        ASSERT_FALSE( document.empty() );
        EXPECT_TRUE( mentions( document, gracePointer ) ) << document;

        const std::string step = rejection_of( negativeExtraGraceDocument );
        ASSERT_FALSE( step.empty() );
        EXPECT_TRUE( mentions( step, extraGracePointer ) ) << step;
    }

    // ── Payloads ─────────────────────────────────────────────

    TEST( Interpreter,
          WaitRequiresADeclaredDuration )
    {
        // time.wait is the one op whose duration is mandatory in JSON; every
        // other Timed op takes its dwell from defaulted options.
        const std::string message = rejection_of( waitWithoutDuration );
        ASSERT_FALSE( message.empty() );
    }

    TEST( Interpreter,
          CaptureNeedsExactlyOneTarget )
    {
        const std::string message = rejection_of( captureWithBothTargets );
        ASSERT_FALSE( message.empty() );
    }

    TEST( Interpreter,
          ButtonNamesAndCodesBothLoad )
    {
        const auto parsed = parse( buttonSpellingsDocument );
        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;

        const auto steps = parsed->steps();
        ASSERT_EQ( steps.size(), threeSteps );
        EXPECT_EQ(
            std::get<grab::sequence::ClickCommand>( steps[firstIndex].command ).button,
            middleButton
        );
        EXPECT_EQ(
            std::get<grab::sequence::PressCommand>( steps[secondIndex].command ).button,
            rightButton
        );
        EXPECT_EQ(
            std::get<grab::sequence::ReleaseCommand>( steps[thirdIndex].command ).button,
            wheelDownButton
        );

        expect_round_trips( buttonSpellingsDocument );
    }

    TEST( Interpreter,
          PayloadValuesSurviveTheParse )
    {
        const auto parsed = parse( threeImplicitSteps );
        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;

        const auto steps = parsed->steps();
        ASSERT_EQ( steps.size(), threeSteps );

        const auto& warp =
            std::get<grab::sequence::WarpCommand>( steps[firstIndex].command );
        EXPECT_EQ( warp.to.x, warpX );
        EXPECT_EQ( warp.to.y, warpY );

        const auto& wait =
            std::get<grab::sequence::WaitCommand>( steps[secondIndex].command );
        EXPECT_EQ(
            wait.duration,
            std::chrono::nanoseconds{ std::chrono::milliseconds{ waitMilliseconds } }
        );

        EXPECT_EQ(
            std::get<grab::sequence::ClickCommand>( steps[thirdIndex].command ).button,
            leftButton
        );
    }

    // ── on_error ─────────────────────────────────────────────

    TEST( Interpreter,
          GotoResolvesItsTargetLabel )
    {
        const auto parsed = parse( gotoDocument );
        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;

        ASSERT_EQ( parsed->steps().size(), twoSteps );
        EXPECT_EQ( parsed->steps()[secondIndex].on_error,
                   grab::sequence::ErrorPolicy::Goto );
        EXPECT_EQ( parsed->steps()[secondIndex].on_error_target, recoverLabel );

        expect_round_trips( gotoDocument );
    }

    TEST( Interpreter,
          GotoAtAnUnknownLabelIsRejected )
    {
        const std::string message = rejection_of( gotoNowhereDocument );
        ASSERT_FALSE( message.empty() );
        EXPECT_TRUE( mentions( message, danglingPhrase ) ) << message;
    }

    // ── Document shape ───────────────────────────────────────

    TEST( Interpreter,
          AnEmptyStepListLoadsAsAnEmptySequence )
    {
        const auto parsed = parse( emptyStepsDocument );
        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;
        EXPECT_EQ( parsed->steps().size(), noSteps );
        EXPECT_EQ( parsed->order().size(), noSteps );
    }

    TEST( Interpreter,
          TheStepsFieldIsRequired )
    {
        const std::string message = rejection_of( missingStepsDocument );
        ASSERT_FALSE( message.empty() );
    }

    // ── Round trip ───────────────────────────────────────────

    TEST( Interpreter,
          ParseToJsonParseIsIdenticalIdsIncluded )
    {
        expect_round_trips( everyOpDocument );
    }

    TEST( Interpreter,
          TheSpecExampleParsesAndRoundTrips )
    {
        const auto parsed = parse( specExample );
        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;

        const auto steps = parsed->steps();
        ASSERT_EQ( steps.size(), fiveSteps );
        EXPECT_EQ( parsed->pacing().mode, grab::sequence::PacingMode::Grace );
        EXPECT_EQ( parsed->pacing().grace,
                   std::chrono::milliseconds{ documentGraceMilliseconds } );

        // move -> wait -> click is implicit; shot and type both name click and
        // therefore fork.
        EXPECT_TRUE( steps[firstIndex].after.empty() );
        ASSERT_EQ( steps[secondIndex].after.size(), oneEdge );
        EXPECT_EQ( steps[secondIndex].after.front(), step_id( firstIndex ) );
        ASSERT_EQ( steps[fourthIndex].after.size(), oneEdge );
        EXPECT_EQ( steps[fourthIndex].after.front(), step_id( thirdIndex ) );
        ASSERT_EQ( steps[fifthIndex].after.size(), oneEdge );
        EXPECT_EQ( steps[fifthIndex].after.front(), step_id( thirdIndex ) );

        EXPECT_EQ( steps[fifthIndex].extra_grace,
                   std::chrono::milliseconds{ extraGraceMilliseconds } );

        expect_round_trips( specExample );
    }

    TEST( Interpreter,
          CaptureByLocatorRoundTrips )
    {
        expect_round_trips( captureByLocatorDocument );
    }

    // A Sequence does not have to come from parse(): splice() produces graphs
    // whose predecessors carry no label, and `after` is not expressible as a
    // label there. to_json falls back to the document index, and parse accepts
    // it, so serialization stays total rather than only covering what the
    // grammar happens to have written.
    TEST( Interpreter,
          AnUnlabelledPredecessorSerializesAsADocumentIndex )
    {
        const auto click =
            []( std::string label, std::vector<grab::sequence::StepId> after )
        {
            return grab::sequence::Step{
                .id              = grab::sequence::StepId{},
                .label           = std::move( label ),
                .command         = grab::sequence::Command{ grab::sequence::ClickCommand{
                    .button = leftButton,
                } },
                .after           = std::move( after ),
                .on_error        = grab::sequence::ErrorPolicy::Abort,
                .on_error_target = {},
                .extra_grace     = std::chrono::milliseconds::zero(),
            };
        };

        std::vector<grab::sequence::Step> steps;
        steps.push_back( click( {}, {} ) );
        steps.push_back( click( {}, { step_id( firstIndex ) } ) );
        // Not the preceding step, so `after` must be written out — and step 0
        // has no label to write.
        steps.push_back( click( std::string{ thirdLabel }, { step_id( firstIndex ) } ) );

        auto built = Sequence::build( std::move( steps ),
                                      grab::sequence::PacingOptions{
                                          .mode  = grab::sequence::PacingMode::Strict,
                                          .grace = std::chrono::milliseconds::zero(),
                                      },
                                      std::string{ builtSequenceName } );
        ASSERT_TRUE( built.has_value() ) << built.error().message;

        const auto text = to_json( *built );
        ASSERT_TRUE( text.has_value() ) << text.error().message;
        const auto reparsed = parse( *text );
        ASSERT_TRUE( reparsed.has_value() ) << reparsed.error().message;
        expect_identical( *built, *reparsed );
    }

    TEST( Interpreter,
          AMultiEntryAfterSurvivesTheRoundTrip )
    {
        const auto parsed = parse( everyOpDocument );
        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;

        const auto steps = parsed->steps();
        ASSERT_EQ( steps.size(), grab::sequence::sequenceCommandCount );
        EXPECT_EQ( steps[fifteenthIndex].after.size(), twoEdges );
    }

    // ── load() ───────────────────────────────────────────────

    TEST( Interpreter,
          LoadReadsADocumentFromDisk )
    {
        const auto path = scratch_file( sequenceFileName );
        {
            std::ofstream output{ path, std::ios::binary };
            ASSERT_TRUE( output.is_open() ) << path.string();
            output << specExample;
        }

        const auto      parsed = load( path );
        std::error_code error;
        std::filesystem::remove( path, error );

        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;
        EXPECT_EQ( parsed->steps().size(), fiveSteps );
    }

    TEST( Interpreter,
          LoadNamesTheFileAndThePointerInAnError )
    {
        const auto path = scratch_file( badFileName );
        {
            std::ofstream output{ path, std::ios::binary };
            ASSERT_TRUE( output.is_open() ) << path.string();
            output << unknownOpDocument;
        }

        const auto      parsed = load( path );
        std::error_code error;
        std::filesystem::remove( path, error );

        ASSERT_FALSE( parsed.has_value() );
        const auto& message = parsed.error().message;
        EXPECT_TRUE( mentions( message, path.string() ) ) << message;
        EXPECT_TRUE( mentions( message, opPointer ) ) << message;
        EXPECT_TRUE( mentions( message, unknownOpName ) ) << message;
    }

    TEST( Interpreter,
          LoadOfAMissingFileSaysSo )
    {
        const auto      path = scratch_file( missingFileName );
        std::error_code error;
        std::filesystem::remove( path, error );

        const auto parsed = load( path );
        ASSERT_FALSE( parsed.has_value() );
        EXPECT_TRUE( mentions( parsed.error().message, missingFilePhrase ) )
            << parsed.error().message;
    }

}    // namespace
