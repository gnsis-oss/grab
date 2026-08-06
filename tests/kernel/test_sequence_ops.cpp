// Container operations over a Sequence: splice(), planned(),
// unestimated_steps(), validate().
//
// planned() is an ESTIMATE and must never read as more than one. The only
// duration a document declares is time.wait's; everything else is unknown
// until it is measured, contributes zero, and is counted by
// unestimated_steps() so a caller can say ">= 250 ms, 2 steps unestimated".
//
// Grace is a scheduling property, not a node. Injecting synthetic wait steps
// would change the step count, so the same document would produce different
// StepIds in different pacing modes — which is what the flagship test here
// forbids.

#include "grab/command.hpp"
#include "grab/result.hpp"
#include "grab/sequence_types.hpp"
#include "kernel/sequence/sequence.hpp"
#include "kernel/sequence/sequence_ops.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <chrono>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>
// clang-format on

namespace
{

    using grab::kernel::sequence::planned;
    using grab::kernel::sequence::splice;
    using grab::kernel::sequence::unestimated_steps;
    using grab::kernel::sequence::validate;

    constexpr std::size_t                  noSteps         = 0U;
    constexpr std::size_t                  twoSteps        = 2U;
    constexpr std::size_t                  threeSteps      = 3U;
    constexpr std::size_t                  fourSteps       = 4U;
    constexpr std::size_t                  fiveSteps       = 5U;
    constexpr std::size_t                  twoEdges        = 2U;
    constexpr std::size_t                  fourEdges       = 4U;
    constexpr std::size_t                  threeAncestors  = 3U;
    constexpr std::size_t                  fourUnestimated = 4U;

    constexpr std::chrono::milliseconds    noGrace{ 0 };
    constexpr std::chrono::milliseconds    noExtraGrace{ 0 };
    constexpr std::chrono::milliseconds    graceInterval{ 80 };
    constexpr std::chrono::milliseconds    extraGrace{ 40 };
    constexpr std::chrono::milliseconds    stepWait{ 100 };

    // The diamond: 100 -> {200, 50} -> 25. The critical path runs through the
    // 200 ms branch, so 50 must not appear in the total.
    constexpr std::chrono::milliseconds    rootWait{ 100 };
    constexpr std::chrono::milliseconds    slowBranchWait{ 200 };
    constexpr std::chrono::milliseconds    fastBranchWait{ 50 };
    constexpr std::chrono::milliseconds    tailWait{ 25 };
    constexpr std::chrono::milliseconds    diamondCriticalPath{ 325 };

    // Three 100 ms waits in a chain, under each mode with grace_ms = 80 and
    // extra_grace_ms = 40 on the middle step only. The root never pays grace.
    constexpr std::chrono::milliseconds    strictTotal{ 300 };
    constexpr std::chrono::milliseconds    graceTotal{ 460 };
    constexpr std::chrono::milliseconds    preciseTotal{ 500 };

    const std::string                      hostName            = "host";
    const std::string                      insertName          = "insert";
    const std::string                      firstLabel          = "first";
    const std::string                      secondLabel         = "second";
    const std::string                      thirdLabel          = "third";
    const std::string                      fourthLabel         = "fourth";
    const std::string                      injectedFirstLabel  = "injected-first";
    const std::string                      injectedSecondLabel = "injected-second";

    constexpr grab::sequence::StepId::Half firstIndex          = 0U;
    constexpr grab::sequence::StepId::Half secondIndex         = 1U;
    constexpr grab::sequence::StepId::Half thirdIndex          = 2U;
    constexpr grab::sequence::StepId::Half fourthIndex         = 3U;
    constexpr grab::sequence::StepId::Half fifthIndex          = 4U;
    constexpr grab::sequence::StepId::Half absentIndex         = 9U;

    [[nodiscard]]
    grab::sequence::StepId
    step_id( grab::sequence::StepId::Half index )
    {
        return grab::sequence::StepId{ index, grab::sequence::StepId::firstGeneration };
    }

    [[nodiscard]]
    grab::sequence::Step
    click_step( std::string                         label,
                std::vector<grab::sequence::StepId> after )
    {
        return grab::sequence::Step{
            .label   = std::move( label ),
            .command = grab::sequence::ClickCommand{},
            .after   = std::move( after ),
        };
    }

    [[nodiscard]]
    grab::sequence::Step
    wait_step( std::string                         label,
               std::chrono::milliseconds           duration,
               std::vector<grab::sequence::StepId> after,
               std::chrono::milliseconds           extra )
    {
        return grab::sequence::Step{
            .label       = std::move( label ),
            .command     = grab::sequence::WaitCommand{ .duration = duration },
            .after       = std::move( after ),
            .extra_grace = extra,
        };
    }

    [[nodiscard]]
    grab::Result<grab::kernel::sequence::Sequence>
    build( std::vector<grab::sequence::Step> steps,
           grab::sequence::PacingOptions     pacing,
           std::string                       name )
    {
        return grab::kernel::sequence::Sequence::build( std::move( steps ),
                                                        pacing,
                                                        std::move( name ) );
    }

    [[nodiscard]]
    grab::Result<grab::kernel::sequence::Sequence>
    build_strict( std::vector<grab::sequence::Step> steps,
                  std::string                       name )
    {
        return build( std::move( steps ),
                      grab::sequence::PacingOptions{},
                      std::move( name ) );
    }

    // One 100 ms wait per step, chained, with extra grace declared on the
    // middle step. The SAME step list feeds all three pacing modes.
    [[nodiscard]]
    std::vector<grab::sequence::Step>
    paced_document()
    {
        std::vector<grab::sequence::Step> steps;
        steps.push_back( wait_step( firstLabel, stepWait, {}, noExtraGrace ) );
        steps.push_back(
            wait_step( secondLabel, stepWait, { step_id( firstIndex ) }, extraGrace )
        );
        steps.push_back(
            wait_step( thirdLabel, stepWait, { step_id( secondIndex ) }, noExtraGrace )
        );
        return steps;
    }

    // ── planned() and unestimated_steps() ─────────────────

    TEST( SequenceOps,
          PlannedIsZeroForAnEmptyDocument )
    {
        auto built = build_strict( {}, hostName );
        ASSERT_TRUE( built.has_value() ) << built.error().message;
        EXPECT_EQ( planned( *built ), std::chrono::nanoseconds::zero() );
        EXPECT_EQ( unestimated_steps( *built ), noSteps );
    }

    // The critical path, not the sum: the 50 ms branch runs concurrently with
    // the 200 ms one and contributes nothing to the total.
    TEST( SequenceOps,
          PlannedIsTheCriticalPathOverDeclaredDurations )
    {
        auto built =
            build_strict( { wait_step( firstLabel, rootWait, {}, noExtraGrace ),
                            wait_step( secondLabel,
                                       slowBranchWait,
                                       { step_id( firstIndex ) },
                                       noExtraGrace ),
                            wait_step( thirdLabel,
                                       fastBranchWait,
                                       { step_id( firstIndex ) },
                                       noExtraGrace ),
                            wait_step( fourthLabel,
                                       tailWait,
                                       { step_id( secondIndex ), step_id( thirdIndex ) },
                                       noExtraGrace ) },
                          hostName );
        ASSERT_TRUE( built.has_value() ) << built.error().message;
        EXPECT_EQ( built->steps().size(), fourSteps );
        EXPECT_EQ( planned( *built ), std::chrono::nanoseconds{ diamondCriticalPath } );
        EXPECT_EQ( unestimated_steps( *built ), noSteps );
    }

    // A click has no declared duration — it is UNKNOWN, not zero. It
    // contributes zero to the estimate and is counted, so the caller can say
    // ">= 100 ms, 2 steps unestimated" instead of presenting 100 ms as fact.
    TEST( SequenceOps,
          UnknownDurationsContributeZeroAndAreCounted )
    {
        auto built =
            build_strict( { click_step( firstLabel, {} ),
                            wait_step( secondLabel,
                                       stepWait,
                                       { step_id( firstIndex ) },
                                       noExtraGrace ),
                            click_step( thirdLabel, { step_id( secondIndex ) } ) },
                          hostName );
        ASSERT_TRUE( built.has_value() ) << built.error().message;
        EXPECT_EQ( planned( *built ), std::chrono::nanoseconds{ stepWait } );
        EXPECT_EQ( unestimated_steps( *built ), twoSteps );
    }

    // THE property that forbids implementing grace as injected wait nodes.
    // Injected nodes would change the step count, and identity is positional,
    // so the same document would carry different StepIds per mode — breaking
    // the JSON round-trip and every id a report or a goto_step ever quoted.
    TEST( SequenceOps,
          PacingModesShareIdsAndDifferInPlannedTotal )
    {
        auto strict  = build( paced_document(),
                              grab::sequence::PacingOptions{
                                  .mode  = grab::sequence::PacingMode::Strict,
                                  .grace = graceInterval
                              },
                              hostName );
        auto graced  = build( paced_document(),
                              grab::sequence::PacingOptions{
                                  .mode  = grab::sequence::PacingMode::Grace,
                                  .grace = graceInterval
                              },
                              hostName );
        auto precise = build( paced_document(),
                              grab::sequence::PacingOptions{
                                  .mode  = grab::sequence::PacingMode::Precise,
                                  .grace = graceInterval
                              },
                              hostName );
        ASSERT_TRUE( strict.has_value() ) << strict.error().message;
        ASSERT_TRUE( graced.has_value() ) << graced.error().message;
        ASSERT_TRUE( precise.has_value() ) << precise.error().message;

        ASSERT_EQ( strict->steps().size(), threeSteps );
        ASSERT_EQ( graced->steps().size(), threeSteps );
        ASSERT_EQ( precise->steps().size(), threeSteps );

        for( std::size_t index = 0U; index < threeSteps; ++index )
        {
            EXPECT_EQ( strict->steps()[index].id, graced->steps()[index].id );
            EXPECT_EQ( strict->steps()[index].id, precise->steps()[index].id );
            EXPECT_EQ( strict->order()[index], graced->order()[index] );
            EXPECT_EQ( strict->order()[index], precise->order()[index] );
        }

        // Same ids, three different plans: 300 back to back, +80 on each of
        // the two non-root steps, +40 more where the author asked for it.
        EXPECT_EQ( planned( *strict ), std::chrono::nanoseconds{ strictTotal } );
        EXPECT_EQ( planned( *graced ), std::chrono::nanoseconds{ graceTotal } );
        EXPECT_EQ( planned( *precise ), std::chrono::nanoseconds{ preciseTotal } );

        // Grace is known before the run, so it feeds the estimate rather than
        // being discovered as overrun.
        EXPECT_GT( planned( *graced ), planned( *strict ) );
        EXPECT_GT( planned( *precise ), planned( *graced ) );
    }

    // Roots start immediately: grace is the gap BETWEEN steps, and a document
    // whose single step is a root pays nothing for it.
    TEST( SequenceOps,
          GraceDoesNotDelayARoot )
    {
        auto built = build( { wait_step( firstLabel, stepWait, {}, extraGrace ) },
                            grab::sequence::PacingOptions{
                                .mode  = grab::sequence::PacingMode::Precise,
                                .grace = graceInterval
                            },
                            hostName );
        ASSERT_TRUE( built.has_value() ) << built.error().message;
        EXPECT_EQ( planned( *built ), std::chrono::nanoseconds{ stepWait } );
    }

    // The mode is the SOLE authority. extra_grace_ms loads under grace and is
    // ignored rather than rejected — otherwise one document could not run
    // under all three modes unedited.
    TEST( SequenceOps,
          ExtraGraceIsIgnoredOutsidePreciseMode )
    {
        const grab::sequence::PacingOptions graceMode{
            .mode  = grab::sequence::PacingMode::Grace,
            .grace = graceInterval
        };

        auto declared = build( paced_document(), graceMode, hostName );
        ASSERT_TRUE( declared.has_value() ) << declared.error().message;

        auto plainSteps = paced_document();
        for( auto& step : plainSteps )
        {
            step.extra_grace = noExtraGrace;
        }
        auto plain = build( std::move( plainSteps ), graceMode, hostName );
        ASSERT_TRUE( plain.has_value() ) << plain.error().message;

        EXPECT_EQ( planned( *declared ), planned( *plain ) );
        EXPECT_EQ( planned( *declared ), std::chrono::nanoseconds{ graceTotal } );
    }

    // `grab play --pacing`/`--grace-ms` override the document, so the estimate
    // has to be computable under pacing the document never carried.
    TEST( SequenceOps,
          PlannedFollowsCallerSuppliedPacing )
    {
        auto built = build( paced_document(),
                            grab::sequence::PacingOptions{
                                .mode  = grab::sequence::PacingMode::Strict,
                                .grace = noGrace
                            },
                            hostName );
        ASSERT_TRUE( built.has_value() ) << built.error().message;

        EXPECT_EQ( planned( *built ), std::chrono::nanoseconds{ strictTotal } );
        EXPECT_EQ( planned( *built,
                            grab::sequence::PacingOptions{
                                .mode  = grab::sequence::PacingMode::Grace,
                                .grace = graceInterval
                            } ),
                   std::chrono::nanoseconds{ graceTotal } );
        EXPECT_EQ( planned( *built,
                            grab::sequence::PacingOptions{
                                .mode  = grab::sequence::PacingMode::Precise,
                                .grace = graceInterval
                            } ),
                   std::chrono::nanoseconds{ preciseTotal } );
    }

    // ── splice() ──────────────────────────────────────────

    TEST( SequenceOps,
          SpliceKeepsExistingIdsValidAndStaysAtGenerationOne )
    {
        auto host =
            build_strict( { click_step( firstLabel, {} ),
                            click_step( secondLabel, { step_id( firstIndex ) } ),
                            click_step( thirdLabel, { step_id( secondIndex ) } ) },
                          hostName );
        ASSERT_TRUE( host.has_value() ) << host.error().message;

        auto insert = build_strict(
            { wait_step( injectedFirstLabel, stepWait, {}, noExtraGrace ),
              click_step( injectedSecondLabel, { step_id( firstIndex ) } ) },
            insertName
        );
        ASSERT_TRUE( insert.has_value() ) << insert.error().message;

        auto spliced = splice( *host, step_id( firstIndex ), *insert );
        ASSERT_TRUE( spliced.has_value() ) << spliced.error().message;
        EXPECT_EQ( spliced->steps().size(), fiveSteps );

        // Every id the host handed out still names the step it named before.
        ASSERT_TRUE( spliced->resolve_label( firstLabel ).has_value() );
        EXPECT_EQ( spliced->resolve_label( firstLabel )->index(), firstIndex );
        EXPECT_EQ( spliced->resolve_label( secondLabel )->index(), secondIndex );
        EXPECT_EQ( spliced->resolve_label( thirdLabel )->index(), thirdIndex );

        // The injected steps took fresh indices above the high-water mark.
        EXPECT_EQ( spliced->resolve_label( injectedFirstLabel )->index(), fourthIndex );
        EXPECT_EQ( spliced->resolve_label( injectedSecondLabel )->index(), fifthIndex );

        // No index was reused, so nothing bumps the generation half. It is a
        // reservation for a future remove(), not an active mechanism.
        for( const auto& step : spliced->steps() )
        {
            EXPECT_EQ( step.id.generation(), grab::sequence::StepId::firstGeneration );
        }

        // The host's pacing and name survive; the insert's are dropped.
        EXPECT_EQ( spliced->name(), hostName );
        EXPECT_EQ( spliced->pacing().mode, host->pacing().mode );
    }

    // In series, not beside: A -> B with X spliced at A becomes A -> X -> B.
    // The parallel reading would run injected input concurrently with the
    // input it was injected before.
    TEST( SequenceOps,
          SpliceRewiresTheAnchorSuccessorsOntoTheInjectedSink )
    {
        auto host =
            build_strict( { click_step( firstLabel, {} ),
                            click_step( secondLabel, { step_id( firstIndex ) } ),
                            click_step( thirdLabel, { step_id( secondIndex ) } ) },
                          hostName );
        ASSERT_TRUE( host.has_value() ) << host.error().message;

        auto insert = build_strict(
            { wait_step( injectedFirstLabel, stepWait, {}, noExtraGrace ),
              click_step( injectedSecondLabel, { step_id( firstIndex ) } ) },
            insertName
        );
        ASSERT_TRUE( insert.has_value() ) << insert.error().message;

        auto spliced = splice( *host, step_id( firstIndex ), *insert );
        ASSERT_TRUE( spliced.has_value() ) << spliced.error().message;

        const auto& graph = spliced->graph();
        EXPECT_EQ( graph.edge_count(), fourEdges );
        EXPECT_TRUE( graph.contains_edge( step_id( firstIndex ),
                                          step_id( fourthIndex ) ) );
        EXPECT_TRUE( graph.contains_edge( step_id( fourthIndex ),
                                          step_id( fifthIndex ) ) );
        EXPECT_TRUE( graph.contains_edge( step_id( fifthIndex ),
                                          step_id( secondIndex ) ) );
        EXPECT_TRUE( graph.contains_edge( step_id( secondIndex ),
                                          step_id( thirdIndex ) ) );
        EXPECT_FALSE( graph.contains_edge( step_id( firstIndex ),
                                           step_id( secondIndex ) ) );

        // Ancestry follows the rewiring, which is what goto_step reads.
        const auto ancestors = spliced->ancestors_of( step_id( secondIndex ) );
        ASSERT_EQ( ancestors.size(), threeAncestors );

        // The injected wait now sits on the critical path.
        EXPECT_EQ( planned( *spliced ), std::chrono::nanoseconds{ stepWait } );
        EXPECT_EQ( unestimated_steps( *spliced ), fourUnestimated );
    }

    TEST( SequenceOps,
          SpliceRejectsCollidingLabels )
    {
        auto host = build_strict( { click_step( firstLabel, {} ) }, hostName );
        ASSERT_TRUE( host.has_value() ) << host.error().message;

        auto insert = build_strict( { click_step( firstLabel, {} ) }, insertName );
        ASSERT_TRUE( insert.has_value() ) << insert.error().message;

        const auto spliced = splice( *host, step_id( firstIndex ), *insert );
        ASSERT_FALSE( spliced.has_value() );
        EXPECT_EQ( spliced.error().code, grab::ErrorCode::InvalidArgument );
        EXPECT_NE( spliced.error().message.find( firstLabel ), std::string::npos )
            << spliced.error().message;
        EXPECT_NE( spliced.error().message.find( "already exists" ), std::string::npos )
            << spliced.error().message;
    }

    TEST( SequenceOps,
          SpliceRejectsAnAnchorOutsideTheHost )
    {
        auto host = build_strict( { click_step( firstLabel, {} ) }, hostName );
        ASSERT_TRUE( host.has_value() ) << host.error().message;

        auto insert = build_strict( { click_step( secondLabel, {} ) }, insertName );
        ASSERT_TRUE( insert.has_value() ) << insert.error().message;

        const auto missing = splice( *host, step_id( absentIndex ), *insert );
        ASSERT_FALSE( missing.has_value() );
        EXPECT_NE( missing.error().message.find( "no such step" ), std::string::npos )
            << missing.error().message;

        const auto nil = splice( *host, grab::sequence::StepId::nil(), *insert );
        ASSERT_FALSE( nil.has_value() );
    }

    TEST( SequenceOps,
          SpliceOfAnEmptyInsertIsACopy )
    {
        auto host =
            build_strict( { click_step( firstLabel, {} ),
                            click_step( secondLabel, { step_id( firstIndex ) } ),
                            click_step( thirdLabel, { step_id( secondIndex ) } ) },
                          hostName );
        ASSERT_TRUE( host.has_value() ) << host.error().message;

        auto insert = build_strict( {}, insertName );
        ASSERT_TRUE( insert.has_value() ) << insert.error().message;

        auto spliced = splice( *host, step_id( firstIndex ), *insert );
        ASSERT_TRUE( spliced.has_value() ) << spliced.error().message;
        EXPECT_EQ( spliced->steps().size(), threeSteps );
        EXPECT_EQ( spliced->graph().edge_count(), twoEdges );
        EXPECT_TRUE( spliced->graph().contains_edge( step_id( firstIndex ),
                                                     step_id( secondIndex ) ) );
    }

    // ── validate() ────────────────────────────────────────

    // A DECLARED pass-through, not an unimplemented one. The document below
    // claims the pointer from two parallel branches at once — precisely the
    // resource rule that will land here one day — and validate() accepts it
    // today, because grab runs what the author wrote.
    TEST( SequenceOps,
          ValidateAcceptsEveryDocumentIncludingOneAFutureRuleWouldReject )
    {
        const grab::kernel::sequence::Sequence defaulted;
        EXPECT_TRUE( validate( defaulted ).has_value() );

        auto empty = build_strict( {}, hostName );
        ASSERT_TRUE( empty.has_value() ) << empty.error().message;
        EXPECT_TRUE( validate( *empty ).has_value() );

        std::vector<grab::sequence::Step> contended;
        contended.push_back( click_step( firstLabel, {} ) );
        contended.push_back( grab::sequence::Step{
            .label   = secondLabel,
            .command = grab::sequence::PressCommand{},
            .after   = { step_id( firstIndex ) },
        } );
        contended.push_back( click_step( thirdLabel, { step_id( firstIndex ) } ) );

        auto interleaved = build_strict( std::move( contended ), hostName );
        ASSERT_TRUE( interleaved.has_value() ) << interleaved.error().message;
        EXPECT_TRUE( validate( *interleaved ).has_value() );
    }

}    // namespace
