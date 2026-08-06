// Sequence — the immutable half of the document/run split.
//
// build() is where a document stops being a wish: identity is stamped
// positionally, labels are made unique, dependencies are checked against the
// steps that exist, and the graph is linearised once. Every rejection below
// has to carry its own message, because a loader that answers "invalid
// sequence" leaves the author to bisect the file by hand.

#include "grab/command.hpp"
#include "grab/result.hpp"
#include "grab/sequence_types.hpp"
#include "kernel/sequence/sequence.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace
{

    constexpr std::size_t      noSteps            = 0U;
    constexpr std::size_t      twoSteps           = 2U;
    constexpr std::size_t      fourSteps          = 4U;
    constexpr std::size_t      oneEdge            = 1U;
    constexpr std::size_t      twoAncestors       = 2U;
    constexpr std::size_t      threeAncestors     = 3U;
    constexpr std::size_t      distinctRejections = 6U;
    constexpr std::size_t      oneTooManySteps    = grab::sequence::maxSteps + 1U;

    const std::string          sequenceName       = "test-sequence";
    const std::string          firstLabel         = "first";
    const std::string          secondLabel        = "second";
    const std::string          thirdLabel         = "third";
    const std::string          fourthLabel        = "fourth";
    const std::string          duplicateLabel     = "same";
    constexpr std::string_view missingLabel{ "nowhere" };

    constexpr grab::sequence::StepId::Half firstIndex     = 0U;
    constexpr grab::sequence::StepId::Half secondIndex    = 1U;
    constexpr grab::sequence::StepId::Half thirdIndex     = 2U;
    constexpr grab::sequence::StepId::Half fourthIndex    = 3U;
    constexpr grab::sequence::StepId::Half absentIndex    = 9U;
    constexpr grab::sequence::StepId::Half nextGeneration = 2U;

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
    grab::Result<grab::kernel::sequence::Sequence>
    build( std::vector<grab::sequence::Step> steps )
    {
        return grab::kernel::sequence::Sequence::build( std::move( steps ),
                                                        grab::sequence::PacingOptions{},
                                                        sequenceName );
    }

    // ── build(), the accepting path ────────────────────────

    TEST( Sequence,
          BuildsATwoStepChain )
    {
        auto built = build(
            { click_step( firstLabel, {} ),
              click_step( secondLabel, { step_id( firstIndex ) } ) }
        );
        ASSERT_TRUE( built.has_value() ) << built.error().message;
        EXPECT_EQ( built->steps().size(), twoSteps );
        EXPECT_EQ( built->order().size(), twoSteps );
        EXPECT_EQ( built->order()[0].index(), firstIndex );
        EXPECT_EQ( built->order()[1].index(), secondIndex );
        EXPECT_EQ( built->name(), sequenceName );

        const auto resolved = built->resolve_label( secondLabel );
        ASSERT_TRUE( resolved.has_value() );
        EXPECT_EQ( resolved->index(), secondIndex );
        EXPECT_FALSE( built->resolve_label( missingLabel ).has_value() );

        // Every live id is generation 1: nothing in this design ever bumps it.
        for( const auto& step : built->steps() )
        {
            EXPECT_EQ( step.id.generation(), grab::sequence::StepId::firstGeneration );
        }
        ASSERT_NE( built->find( step_id( firstIndex ) ), nullptr );
        EXPECT_EQ( built->find( step_id( absentIndex ) ), nullptr );
    }

    // An empty document is a document. It is what an author gets from a JSON
    // file whose "steps" array is empty, and it must not be an error — a
    // sequence with nothing to do simply finishes.
    TEST( Sequence,
          BuildsAnEmptyDocument )
    {
        auto built = build( {} );
        ASSERT_TRUE( built.has_value() ) << built.error().message;
        EXPECT_EQ( built->steps().size(), noSteps );
        EXPECT_EQ( built->order().size(), noSteps );
        EXPECT_EQ( built->find( step_id( firstIndex ) ), nullptr );
    }

    // find() matches the WHOLE id, not the index half. The generation is inert
    // today, but an id that names a different generation names a different
    // step, and answering it with a pointer would be the bug the field exists
    // to prevent.
    TEST( Sequence,
          FindRejectsAStaleGeneration )
    {
        auto built = build( { click_step( firstLabel, {} ) } );
        ASSERT_TRUE( built.has_value() ) << built.error().message;

        ASSERT_NE( built->find( step_id( firstIndex ) ), nullptr );
        EXPECT_EQ( built->find( grab::sequence::StepId{ firstIndex, nextGeneration } ),
                   nullptr );
        EXPECT_EQ( built->find( grab::sequence::StepId::nil() ), nullptr );
    }

    // ── copying ───────────────────────────────────────────

    // AdjacencyGraph's copy constructor is deleted on purpose, so Sequence
    // hand-writes one that rebuilds the graph and the label map from steps_.
    // A copy that forgot either would still answer steps() correctly and fail
    // only once someone asked about a dependency.
    TEST( Sequence,
          CopyRebuildsTheGraph )
    {
        auto built = build(
            { click_step( firstLabel, {} ),
              click_step( secondLabel, { step_id( firstIndex ) } ) }
        );
        ASSERT_TRUE( built.has_value() ) << built.error().message;

        const grab::kernel::sequence::Sequence copy{ *built };
        EXPECT_EQ( copy.steps().size(), twoSteps );
        EXPECT_EQ( copy.order().size(), twoSteps );
        EXPECT_EQ( copy.graph().node_count(), twoSteps );
        EXPECT_EQ( copy.graph().edge_count(), oneEdge );
        EXPECT_TRUE( copy.graph().contains_edge( step_id( firstIndex ),
                                                 step_id( secondIndex ) ) );
        ASSERT_TRUE( copy.resolve_label( firstLabel ).has_value() );
        EXPECT_EQ( copy.resolve_label( firstLabel )->index(), firstIndex );
        EXPECT_EQ( copy.name(), sequenceName );

        // The source is untouched — a copy is a copy, not a move.
        EXPECT_EQ( built->graph().edge_count(), oneEdge );
    }

    TEST( Sequence,
          CopyAssignmentRebuildsTheGraph )
    {
        auto built = build(
            { click_step( firstLabel, {} ),
              click_step( secondLabel, { step_id( firstIndex ) } ) }
        );
        ASSERT_TRUE( built.has_value() ) << built.error().message;

        grab::kernel::sequence::Sequence assigned;
        assigned = *built;
        EXPECT_EQ( assigned.steps().size(), twoSteps );
        EXPECT_EQ( assigned.graph().edge_count(), oneEdge );
        EXPECT_TRUE( assigned.graph().contains_edge( step_id( firstIndex ),
                                                     step_id( secondIndex ) ) );
        ASSERT_TRUE( assigned.resolve_label( secondLabel ).has_value() );

        // Self-assignment must not clear the graph on the way through.
        const auto& alias = assigned;
        assigned          = alias;
        EXPECT_EQ( assigned.graph().edge_count(), oneEdge );
    }

    // ── build(), the six rejections ───────────────────────

    TEST( Sequence,
          RejectsACycle )
    {
        const auto built = build(
            { click_step( firstLabel, { step_id( secondIndex ) } ),
              click_step( secondLabel, { step_id( firstIndex ) } ) }
        );
        ASSERT_FALSE( built.has_value() );
        EXPECT_EQ( built.error().code, grab::ErrorCode::InvalidArgument );
        EXPECT_NE( built.error().message.find( "cycle" ), std::string::npos )
            << built.error().message;
    }

    TEST( Sequence,
          RejectsADanglingDependency )
    {
        const auto built =
            build( { click_step( firstLabel, { step_id( absentIndex ) } ) } );
        ASSERT_FALSE( built.has_value() );
        EXPECT_NE( built.error().message.find( "does not exist" ), std::string::npos )
            << built.error().message;
        // The message has to name the offending step, or the author is left
        // bisecting the file.
        EXPECT_NE( built.error().message.find( firstLabel ), std::string::npos )
            << built.error().message;
    }

    // THE case the graph cannot catch for itself: add_edge returns false for
    // source == target and the edge simply never enters the graph, so the
    // topological sort sees an acyclic graph and succeeds. Were build() to
    // ignore that false, "after": [self] would load as valid with its
    // dependency silently dropped.
    TEST( Sequence,
          RejectsASelfEdge )
    {
        const auto built =
            build( { click_step( firstLabel, { step_id( firstIndex ) } ) } );
        ASSERT_FALSE( built.has_value() );
        EXPECT_NE( built.error().message.find( "itself" ), std::string::npos )
            << built.error().message;
    }

    // Same shape, same cause: a duplicate edge is refused by add_edge, so the
    // second entry would vanish rather than fail.
    TEST( Sequence,
          RejectsARepeatedDependency )
    {
        const auto built = build(
            { click_step( firstLabel, {} ),
              click_step( secondLabel,
                          { step_id( firstIndex ), step_id( firstIndex ) } ) }
        );
        ASSERT_FALSE( built.has_value() );
        EXPECT_NE( built.error().message.find( "twice" ), std::string::npos )
            << built.error().message;
    }

    TEST( Sequence,
          RejectsADuplicateLabel )
    {
        const auto built = build(
            { click_step( duplicateLabel, {} ), click_step( duplicateLabel, {} ) }
        );
        ASSERT_FALSE( built.has_value() );
        EXPECT_NE(
            built.error().message.find( "duplicate step label" ),
            std::string::npos
        ) << built.error().message;
    }

    // maxSteps is 65536, not 65535: index 0 is usable because nil is (0,0) and
    // generation starts at 1. One past it must not wrap the 16-bit index half.
    TEST( Sequence,
          RejectsMoreThanMaxSteps )
    {
        std::vector<grab::sequence::Step> steps;
        steps.reserve( oneTooManySteps );
        for( std::size_t index = 0U; index < oneTooManySteps; ++index )
        {
            steps.push_back( click_step( {}, {} ) );
        }

        const auto built = build( std::move( steps ) );
        ASSERT_FALSE( built.has_value() );
        EXPECT_NE(
            built.error().message.find( std::to_string( grab::sequence::maxSteps ) ),
            std::string::npos
        ) << built.error().message;
    }

    // Six rejections, six messages. Sharing one would make the loader's answer
    // useless in exactly the cases the author most needs it.
    TEST( Sequence,
          RejectionMessagesAreAllDistinct )
    {
        std::vector<grab::sequence::Step> tooMany;
        tooMany.reserve( oneTooManySteps );
        for( std::size_t index = 0U; index < oneTooManySteps; ++index )
        {
            tooMany.push_back( click_step( {}, {} ) );
        }

        std::vector<grab::Result<grab::kernel::sequence::Sequence>> rejected;
        rejected.push_back( build(
            { click_step( firstLabel, { step_id( secondIndex ) } ),
              click_step( secondLabel, { step_id( firstIndex ) } ) }
        ) );
        rejected.push_back( build( { click_step( firstLabel,
                                                 { step_id( absentIndex ) } ) } ) );
        rejected.push_back( build( { click_step( firstLabel,
                                                 { step_id( firstIndex ) } ) } ) );
        rejected.push_back( build(
            { click_step( firstLabel, {} ),
              click_step( secondLabel,
                          { step_id( firstIndex ), step_id( firstIndex ) } ) }
        ) );
        rejected.push_back( build(
            { click_step( duplicateLabel, {} ), click_step( duplicateLabel, {} ) }
        ) );
        rejected.push_back( build( std::move( tooMany ) ) );

        std::set<std::string> messages;
        for( const auto& outcome : rejected )
        {
            ASSERT_FALSE( outcome.has_value() );
            messages.insert( outcome.error().message );
        }
        EXPECT_EQ( messages.size(), distinctRejections );
    }

    // ── ancestry ──────────────────────────────────────────

    // A diamond: 0 -> {1,2} -> 3. goto_step is defined over ancestors rather
    // than over a prefix of order(), so ancestors_of has to be exact.
    TEST( Sequence,
          ReportsAncestorsOfADiamondTail )
    {
        auto built = build(
            { click_step( firstLabel, {} ),
              click_step( secondLabel, { step_id( firstIndex ) } ),
              click_step( thirdLabel, { step_id( firstIndex ) } ),
              click_step( fourthLabel,
                          { step_id( secondIndex ), step_id( thirdIndex ) } ) }
        );
        ASSERT_TRUE( built.has_value() ) << built.error().message;
        EXPECT_EQ( built->steps().size(), fourSteps );

        const auto ancestors = built->ancestors_of( step_id( fourthIndex ) );
        ASSERT_EQ( ancestors.size(), threeAncestors );
        EXPECT_EQ( ancestors[0], step_id( firstIndex ) );
        EXPECT_EQ( ancestors[1], step_id( secondIndex ) );
        EXPECT_EQ( ancestors[2], step_id( thirdIndex ) );

        // A root has none, and a node outside the document has none either.
        EXPECT_TRUE( built->ancestors_of( step_id( firstIndex ) ).empty() );
        EXPECT_TRUE( built->ancestors_of( step_id( absentIndex ) ).empty() );
    }

    // The trap this method exists to avoid. order() is ONE arbitrary Kahn
    // linearization: here it is [0, 2, 1, 3], so the prefix before step 3
    // contains step 2 — an unrelated parallel root that is not an ancestor of
    // anything. goto_step defined over that prefix would skip a branch the
    // author never mentioned.
    TEST( Sequence,
          AncestorsExcludeUnrelatedParallelBranches )
    {
        auto built = build(
            { click_step( firstLabel, {} ),
              click_step( secondLabel, { step_id( firstIndex ) } ),
              click_step( thirdLabel, {} ),
              click_step( fourthLabel, { step_id( secondIndex ) } ) }
        );
        ASSERT_TRUE( built.has_value() ) << built.error().message;

        const auto order = built->order();
        ASSERT_EQ( order.size(), fourSteps );
        // The unrelated root really does sit inside the prefix — without this
        // the test below would pass for the wrong reason.
        EXPECT_EQ( order[1].index(), thirdIndex );

        const auto ancestors = built->ancestors_of( step_id( fourthIndex ) );
        ASSERT_EQ( ancestors.size(), twoAncestors );
        EXPECT_EQ( ancestors[0], step_id( firstIndex ) );
        EXPECT_EQ( ancestors[1], step_id( secondIndex ) );
        EXPECT_EQ( std::ranges::find( ancestors, step_id( thirdIndex ) ),
                   ancestors.end() );
    }

}    // namespace
