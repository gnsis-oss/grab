// PLACEHOLDER — registered in tests/CMakeLists.txt by Phase 0 so the sequence
// unit can replace this file without touching a shared build file.
//
// Sequence::build is the one thing Phase 0 actually implements, so this file
// covers it rather than only proving the header parses. The sequence unit
// replaces these with the full suite (planned(), splice(), to_json, pacing).

#include "grab/command.hpp"
#include "grab/sequence_types.hpp"
#include "kernel/sequence/sequence.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace
{

    constexpr std::size_t                  twoSteps       = 2U;
    constexpr std::size_t                  fourSteps      = 4U;
    constexpr std::size_t                  oneEdge        = 1U;
    constexpr std::size_t                  threeAncestors = 3U;

    const std::string                      sequenceName   = "placeholder";
    const std::string                      firstLabel     = "first";
    const std::string                      secondLabel    = "second";
    const std::string                      duplicateLabel = "same";
    constexpr std::string_view             missingLabel{ "nowhere" };

    constexpr grab::sequence::StepId::Half firstIndex  = 0U;
    constexpr grab::sequence::StepId::Half secondIndex = 1U;
    constexpr grab::sequence::StepId::Half thirdIndex  = 2U;
    constexpr grab::sequence::StepId::Half fourthIndex = 3U;
    constexpr grab::sequence::StepId::Half absentIndex = 9U;

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

    TEST( Placeholder,
          SequenceBuildsATwoStepChain )
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

        const auto resolved = built->resolve_label( secondLabel );
        ASSERT_TRUE( resolved.has_value() );
        EXPECT_EQ( resolved->index(), secondIndex );
        EXPECT_FALSE( built->resolve_label( missingLabel ).has_value() );

        // Every live id is generation 1: nothing in this design ever bumps it.
        EXPECT_EQ( built->order()[0].generation(),
                   grab::sequence::StepId::firstGeneration );
        ASSERT_NE( built->find( step_id( firstIndex ) ), nullptr );
        EXPECT_EQ( built->find( step_id( absentIndex ) ), nullptr );
    }

    // AdjacencyGraph's copy constructor is deleted, so Sequence hand-writes one
    // that rebuilds the graph from steps_.
    TEST( Placeholder,
          SequenceCopyRebuildsTheGraph )
    {
        auto built = build(
            { click_step( firstLabel, {} ),
              click_step( secondLabel, { step_id( firstIndex ) } ) }
        );
        ASSERT_TRUE( built.has_value() ) << built.error().message;

        const grab::kernel::sequence::Sequence copy{ *built };
        EXPECT_EQ( copy.steps().size(), twoSteps );
        EXPECT_EQ( copy.graph().edge_count(), oneEdge );
        EXPECT_TRUE( copy.graph().contains_edge( step_id( firstIndex ),
                                                 step_id( secondIndex ) ) );
        ASSERT_TRUE( copy.resolve_label( firstLabel ).has_value() );
    }

    TEST( Placeholder,
          SequenceRejectsADanglingDependency )
    {
        const auto built =
            build( { click_step( firstLabel, { step_id( absentIndex ) } ) } );
        ASSERT_FALSE( built.has_value() );
        EXPECT_NE( built.error().message.find( "does not exist" ), std::string::npos )
            << built.error().message;
    }

    // add_edge returns false for a self-loop and the edge never enters the
    // graph, so the topological sort cannot see it. The loader has to reject it.
    TEST( Placeholder,
          SequenceRejectsASelfEdge )
    {
        const auto built =
            build( { click_step( firstLabel, { step_id( firstIndex ) } ) } );
        ASSERT_FALSE( built.has_value() );
        EXPECT_NE( built.error().message.find( "itself" ), std::string::npos )
            << built.error().message;
    }

    TEST( Placeholder,
          SequenceRejectsARepeatedDependency )
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

    TEST( Placeholder,
          SequenceRejectsADuplicateLabel )
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

    TEST( Placeholder,
          SequenceRejectsACycle )
    {
        const auto built = build(
            { click_step( firstLabel, { step_id( secondIndex ) } ),
              click_step( secondLabel, { step_id( firstIndex ) } ) }
        );
        ASSERT_FALSE( built.has_value() );
        EXPECT_NE( built.error().message.find( "cycle" ), std::string::npos )
            << built.error().message;
    }

    // A diamond: 0 -> {1,2} -> 3. goto_step is defined over ancestors rather
    // than over a prefix of order(), so ancestors_of has to be exact.
    TEST( Placeholder,
          SequenceReportsAncestorsOfADiamondTail )
    {
        auto built = build(
            { click_step( {}, {} ),
              click_step( {}, { step_id( firstIndex ) } ),
              click_step( {}, { step_id( firstIndex ) } ),
              click_step( {}, { step_id( secondIndex ), step_id( thirdIndex ) } ) }
        );
        ASSERT_TRUE( built.has_value() ) << built.error().message;
        EXPECT_EQ( built->steps().size(), fourSteps );

        const auto ancestors = built->ancestors_of( step_id( fourthIndex ) );
        ASSERT_EQ( ancestors.size(), threeAncestors );
        EXPECT_EQ( ancestors[0], step_id( firstIndex ) );
        EXPECT_EQ( ancestors[1], step_id( secondIndex ) );
        EXPECT_EQ( ancestors[2], step_id( thirdIndex ) );
        EXPECT_TRUE( built->ancestors_of( step_id( firstIndex ) ).empty() );
    }

}    // namespace
