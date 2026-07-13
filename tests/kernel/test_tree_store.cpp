#include "grab/ids.hpp"
#include "grab/relation.hpp"
#include "grab/result.hpp"
#include "grab/role.hpp"
#include "grab/ui.hpp"
#include "kernel/graph/tree_store.hpp"
#include "spi/tree_source.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
// clang-format on

// NOLINTBEGIN(readability-trailing-comma)
namespace
{

    constexpr grab::RuntimeId runtimeId{ 7U };
    constexpr grab::TreeEpoch treeEpoch{ 2U };

    [[nodiscard]]
    grab::UiNodeRecord
    node( std::uint64_t id,
          grab::RoleId  role     = grab::role::region,
          std::uint32_t states   = 0U,
          std::uint64_t revision = 1U )
    {
        return grab::UiNodeRecord{
            grab::NodeId{id                          },
            grab::NodeGeneration{                  1U },
            role,
            states,
            {                    },
            grab::UiProvenance{
                         .runtime  = runtimeId,.revision = revision,
                         },
        };
    }

    [[nodiscard]]
    grab::UiSnapshot
    snapshot( std::uint64_t                   revision,
              std::vector<grab::UiNodeRecord> nodes,
              std::vector<grab::UiRelation>   relations = {} )
    {
        return grab::UiSnapshot::from_records(
            grab::UiSnapshotMetadata{
                .runtime  = runtimeId,
                .tree     = 3U,
                .epoch    = treeEpoch,
                .revision = revision,
                .complete = true,
            },
            std::move( nodes ),
            {},
            std::move( relations )
        );
    }

    [[nodiscard]]
    grab::spi::UiUpdate
    update( std::uint64_t    sequence,
            grab::UiSnapshot value )
    {
        return grab::spi::UiUpdate{
            .source_sequence = sequence,
            .payload         = std::move( value ),
        };
    }

    [[nodiscard]]
    bool
    has_event( const std::vector<grab::kernel::TreeEvent>& events,
               grab::kernel::TreeEventKind                 kind,
               grab::NodeId                                node_id )
    {
        return std::ranges::any_of(
            events,
            [kind, node_id]( const grab::kernel::TreeEvent& event )
            {
                return event.kind == kind && event.node == node_id;
            }
        );
    }

}    // namespace

TEST( TreeStore,
      AppliesValidatedSnapshot )
{
    grab::kernel::TreeStore store;
    auto                    result =
        store.apply( update( 1U,
                             snapshot( 1U,
                                       {
                                           node( 1U, grab::role::application ),
                                           node( 2U, grab::role::window )
    },
                                       { grab::UiRelation{
                                           .source   = grab::NodeId{ 1U },
                                           .target   = grab::NodeId{ 2U },
                                           .relation = grab::relation::contains,
                                       } } ) ) );

    ASSERT_TRUE( result.has_value() ) << result.error().message;
    EXPECT_EQ( result->previous_revision, 0U );
    EXPECT_EQ( result->revision, 1U );

    const auto current = store.snapshot();
    ASSERT_TRUE( current.has_value() );
    EXPECT_NE( current->node( grab::NodeId{ 2U } ), nullptr );
    ASSERT_EQ( current->roots().size(), 1U );
    EXPECT_EQ( current->roots().front(), grab::NodeId{ 1U } );
}

TEST( TreeStore,
      RejectsUnknownParentWithoutReplacingCurrentGeneration )
{
    grab::kernel::TreeStore store;
    ASSERT_TRUE( store.apply( update( 1U, snapshot( 1U, { node( 1U ) } ) ) ) );

    const auto result =
        store.apply( update( 2U,
                             snapshot( 2U,
                                       {
                                           node( 1U )
    },
                                       { grab::UiRelation{
                                           .source   = grab::NodeId{ 99U },
                                           .target   = grab::NodeId{ 1U },
                                           .relation = grab::relation::contains,
                                       } } ) ) );

    ASSERT_FALSE( result.has_value() );
    EXPECT_EQ( result.error().code, grab::ErrorCode::NoMatch );
    EXPECT_NE( result.error().target.find( "runtime=7" ), std::string::npos );
    EXPECT_NE( result.error().target.find( "revision=2" ), std::string::npos );
    EXPECT_EQ( store.revision(), 1U );
}

TEST( TreeStore,
      RejectsDuplicateNodeId )
{
    grab::kernel::TreeStore store;
    const auto              result =
        store.apply( update( 1U, snapshot( 1U, { node( 1U ), node( 1U ) } ) ) );

    ASSERT_FALSE( result.has_value() );
    EXPECT_EQ( result.error().code, grab::ErrorCode::InvalidArgument );
    EXPECT_NE( result.error().target.find( "revision=1" ), std::string::npos );
    EXPECT_FALSE( store.snapshot().has_value() );
}

TEST( TreeStore,
      RejectsContainsCycle )
{
    grab::kernel::TreeStore store;
    const auto              result =
        store.apply( update( 1U,
                             snapshot( 1U,
                                       {
                                           node( 1U ),
                                           node( 2U )
    },
                                       {
                                           grab::UiRelation{
                                               .source   = grab::NodeId{ 1U },
                                               .target   = grab::NodeId{ 2U },
                                               .relation = grab::relation::contains,
                                           },
                                           grab::UiRelation{
                                               .source   = grab::NodeId{ 2U },
                                               .target   = grab::NodeId{ 1U },
                                               .relation = grab::relation::contains,
                                           },
                                       } ) ) );

    ASSERT_FALSE( result.has_value() );
    EXPECT_EQ( result.error().code, grab::ErrorCode::ProtocolError );
    EXPECT_NE( result.error().message.find( "cycle" ), std::string::npos );
}

TEST( TreeStore,
      PacksTwoCoreRelationsOnOneEdgeAndSupportsReverseLookup )
{
    grab::kernel::TreeStore store;
    const auto              result =
        store.apply( update( 1U,
                             snapshot( 1U,
                                       {
                                           node( 1U ),
                                           node( 2U )
    },
                                       {
                                           grab::UiRelation{
                                               .source   = grab::NodeId{ 1U },
                                               .target   = grab::NodeId{ 2U },
                                               .relation = grab::relation::contains,
                                           },
                                           grab::UiRelation{
                                               .source   = grab::NodeId{ 1U },
                                               .target   = grab::NodeId{ 2U },
                                               .relation = grab::relation::active_child,
                                           },
                                       } ) ) );

    ASSERT_TRUE( result.has_value() ) << result.error().message;
    const auto relation_set =
        store.core_relations( grab::NodeId{ 1U }, grab::NodeId{ 2U } );
    ASSERT_TRUE( relation_set.has_value() );
    EXPECT_NE( *relation_set & grab::kernel::relation_bit( grab::relation::contains ),
               0U );
    EXPECT_NE( *relation_set &
                   grab::kernel::relation_bit( grab::relation::active_child ),
               0U );

    const auto current = store.snapshot();
    ASSERT_TRUE( current.has_value() );
    const auto forward =
        current->related( grab::NodeId{ 1U }, grab::relation::contains );
    ASSERT_EQ( forward.size(), 1U );
    EXPECT_EQ( forward.front(), grab::NodeId{ 2U } );
    const auto reverse =
        current->related_reverse( grab::NodeId{ 2U }, grab::relation::active_child );
    ASSERT_EQ( reverse.size(), 1U );
    EXPECT_EQ( reverse.front(), grab::NodeId{ 1U } );
}

TEST( TreeStore,
      DerivesNodeAndRelationEventsFromBufferedGenerations )
{
    std::vector<grab::kernel::TreeEvent> published;
    grab::kernel::TreeStore              store{
        [&published]( const grab::kernel::TreeEvent& event )
        {
            published.push_back( event );
        },
    };

    ASSERT_TRUE( store.apply( update( 1U,
                                      snapshot( 1U,
                                                {
                                                    node( 1U ),
                                                    node( 2U )
    },
                                                { grab::UiRelation{
                                                    .source   = grab::NodeId{ 1U },
                                                    .target   = grab::NodeId{ 2U },
                                                    .relation = grab::relation::contains,
                                                } } ) ) ) );
    published.clear();

    const auto focused = grab::state_mask( grab::NodeState::Focused );
    const auto result =
        store.apply( update( 2U,
                             snapshot( 2U,
                                       {
                                           node( 1U, grab::role::region, focused, 2U ),
                                           node( 3U )
    },
                                       { grab::UiRelation{
                                           .source   = grab::NodeId{ 1U },
                                           .target   = grab::NodeId{ 3U },
                                           .relation = grab::relation::contains,
                                       } } ) ) );

    ASSERT_TRUE( result.has_value() ) << result.error().message;
    EXPECT_TRUE( has_event( result->events,
                            grab::kernel::TreeEventKind::NodeChanged,
                            grab::NodeId{ 1U } ) );
    EXPECT_TRUE( has_event( result->events,
                            grab::kernel::TreeEventKind::NodeRemoved,
                            grab::NodeId{ 2U } ) );
    EXPECT_TRUE( has_event( result->events,
                            grab::kernel::TreeEventKind::NodeAdded,
                            grab::NodeId{ 3U } ) );
    EXPECT_EQ( published, result->events );
}

TEST( TreeStore,
      PublishesAfterReleasingStoreLock )
{
    grab::kernel::TreeStore* store_pointer = nullptr;
    std::uint64_t            callback_revision{};
    grab::kernel::TreeStore  store{
        [&store_pointer, &callback_revision]( const grab::kernel::TreeEvent& )
        {
            callback_revision = store_pointer->revision();
        },
    };
    store_pointer = &store;

    ASSERT_TRUE( store.apply( update( 1U, snapshot( 1U, { node( 1U ) } ) ) ) );
    EXPECT_EQ( callback_revision, 1U );
}

TEST( TreeStore,
      ReentrantApplyPublishesWholeCommittedBatchesInOrder )
{
    grab::kernel::TreeStore*   store_pointer = nullptr;
    bool                       nested_apply_succeeded{};
    bool                       attempted_nested_apply{};
    std::vector<std::uint64_t> published_revisions;
    grab::kernel::TreeStore    store{
        [&store_pointer,
         &nested_apply_succeeded,
         &attempted_nested_apply,
         &published_revisions]( const grab::kernel::TreeEvent& event )
        {
            published_revisions.push_back( event.revision );
            if( attempted_nested_apply )
            {
                return;
            }

            attempted_nested_apply = true;
            nested_apply_succeeded =
                store_pointer
                    ->apply(
                        update( 2U,
                                snapshot( 2U, { node( 1U ), node( 2U ), node( 3U ) } ) )
                    )
                    .has_value();
        },
    };
    store_pointer = &store;

    ASSERT_TRUE( store.apply( update( 1U,
                                      snapshot( 1U, { node( 1U ), node( 2U ) } ) ) ) );
    ASSERT_TRUE( nested_apply_succeeded );
    ASSERT_GE( published_revisions.size(), 3U );
    EXPECT_EQ( published_revisions.at( 0U ), 1U );
    EXPECT_EQ( published_revisions.at( 1U ), 1U );
    EXPECT_TRUE( std::ranges::all_of( published_revisions.begin() + 2,
                                      published_revisions.end(),
                                      []( std::uint64_t revision )
                                      {
                                          return revision == 2U;
                                      } ) );
    EXPECT_EQ( store.revision(), 2U );
}

TEST( TreeStore,
      RejectsOutOfOrderDeltaAtomically )
{
    grab::kernel::TreeStore store;
    ASSERT_TRUE( store.apply( update( 1U, snapshot( 1U, { node( 1U ) } ) ) ) );

    grab::spi::UiDelta delta{
        .runtime          = runtimeId,
        .tree             = 3U,
        .epoch            = treeEpoch,
        .base_revision    = 0U,
        .revision         = 2U,
        .complete         = true,
        .added_nodes      = { node( 2U, grab::role::region, 0U, 2U ) },
        .changed_nodes    = {},
        .removed_nodes    = {},
        .relation_changes = {},
    };
    const auto result = store.apply( grab::spi::UiUpdate{
        .source_sequence = 2U,
        .payload         = std::move( delta ),
    } );

    ASSERT_FALSE( result.has_value() );
    EXPECT_EQ( result.error().code, grab::ErrorCode::ResyncRequired );
    EXPECT_EQ( store.revision(), 1U );
    const auto current = store.snapshot();
    ASSERT_TRUE( current.has_value() );
    EXPECT_EQ( current->node( grab::NodeId{ 2U } ), nullptr );
}

TEST( TreeStore,
      AppliesSequentialDeltaWithoutDroppingAnotherPackedRelation )
{
    grab::kernel::TreeStore store;
    ASSERT_TRUE(
        store.apply( update( 1U,
                             snapshot( 1U,
                                       {
                                           node( 1U ),
                                           node( 2U )
    },
                                       {
                                           grab::UiRelation{
                                               .source   = grab::NodeId{ 1U },
                                               .target   = grab::NodeId{ 2U },
                                               .relation = grab::relation::contains,
                                           },
                                           grab::UiRelation{
                                               .source   = grab::NodeId{ 1U },
                                               .target   = grab::NodeId{ 2U },
                                               .relation = grab::relation::active_child,
                                           },
                                       } ) ) )
    );

    grab::spi::UiDelta delta{
        .runtime          = runtimeId,
        .tree             = 3U,
        .epoch            = treeEpoch,
        .base_revision    = 1U,
        .revision         = 2U,
        .complete         = true,
        .added_nodes      = {},
        .changed_nodes    = {},
        .removed_nodes    = {},
        .relation_changes = { grab::spi::RelationChange{
            .kind     = grab::spi::RelationChangeKind::Remove,
            .source   = grab::NodeId{ 1U },
            .target   = grab::NodeId{ 2U },
            .relation = grab::relation::contains,
        } },
    };
    const auto result = store.apply( grab::spi::UiUpdate{
        .source_sequence = 2U,
        .payload         = std::move( delta ),
    } );

    ASSERT_TRUE( result.has_value() ) << result.error().message;
    const auto relation_set =
        store.core_relations( grab::NodeId{ 1U }, grab::NodeId{ 2U } );
    ASSERT_TRUE( relation_set.has_value() );
    EXPECT_EQ( *relation_set & grab::kernel::relation_bit( grab::relation::contains ),
               0U );
    EXPECT_NE( *relation_set &
                   grab::kernel::relation_bit( grab::relation::active_child ),
               0U );
    EXPECT_TRUE(
        std::ranges::any_of( result->events,
                             []( const grab::kernel::TreeEvent& event )
                             {
                                 return event.kind ==
                                        grab::kernel::TreeEventKind::RelationRemoved &&
                                        event.relation == grab::relation::contains;
                             } )
    );
}

TEST( TreeStore,
      AcceptsRestartSnapshotWithResetSourceSequence )
{
    grab::kernel::TreeStore store;
    ASSERT_TRUE( store.apply( update( 9U, snapshot( 1U, { node( 1U ) } ) ) ) );

    constexpr grab::RuntimeId restartedRuntime{ 8U };
    const auto                restarted = grab::UiSnapshot::from_records(
        grab::UiSnapshotMetadata{
            .runtime  = restartedRuntime,
            .tree     = 3U,
            .epoch    = grab::TreeEpoch{ 1U },
            .revision = 1U,
            .complete = true,
    },
        { grab::UiNodeRecord{
            grab::NodeId{ 1U },
            grab::NodeGeneration{ 1U },
            grab::role::application,
            0U,
            {},
            grab::UiProvenance{
                .runtime  = restartedRuntime,
                .revision = 1U,
            },
        } },
        { grab::NodeId{ 1U } },
        {}
    );
    const auto result = store.apply( update( 1U, restarted ) );

    ASSERT_TRUE( result.has_value() ) << result.error().message;
    const auto current = store.snapshot();
    ASSERT_TRUE( current.has_value() );
    EXPECT_EQ( current->runtime, restartedRuntime );
    const auto previous = store.previous_snapshot();
    ASSERT_TRUE( previous.has_value() );
    EXPECT_EQ( previous->runtime, runtimeId );
}

TEST( TreeStore,
      RejectsDelayedSnapshotFromRetiredRuntime )
{
    grab::kernel::TreeStore store;
    ASSERT_TRUE( store.apply( update( 9U, snapshot( 1U, { node( 1U ) } ) ) ) );

    constexpr grab::RuntimeId restarted_runtime{ 8U };
    auto                      restarted = grab::UiSnapshot::from_records(
        grab::UiSnapshotMetadata{
            .runtime  = restarted_runtime,
            .tree     = 3U,
            .epoch    = grab::TreeEpoch{ 1U },
            .revision = 1U,
            .complete = true,
    },
        { grab::UiNodeRecord{
            grab::NodeId{ 1U },
            grab::NodeGeneration{ 1U },
            grab::role::application,
            0U,
            {},
            grab::UiProvenance{
                .runtime  = restarted_runtime,
                .revision = 1U,
            },
        } },
        { grab::NodeId{ 1U } },
        {}
    );
    ASSERT_TRUE( store.apply( update( 1U, std::move( restarted ) ) ) );

    const auto delayed =
        store.apply( update( 10U, snapshot( 2U, { node( 1U ), node( 2U ) } ) ) );
    ASSERT_FALSE( delayed.has_value() );
    EXPECT_EQ( delayed.error().code, grab::ErrorCode::RuntimeRestarted );
    const auto current = store.snapshot();
    ASSERT_TRUE( current.has_value() );
    EXPECT_EQ( current->runtime, restarted_runtime );
}

TEST( TreeStore,
      QueueGapRequiresFullSnapshotBeforeAnotherDelta )
{
    grab::kernel::TreeStore store;
    ASSERT_TRUE( store.apply( update( 1U, snapshot( 1U, { node( 1U ) } ) ) ) );

    const auto gap = store.apply( grab::spi::UiUpdate{
        .source_sequence = 2U,
        .payload         = grab::spi::TreeGap{
                                              .runtime              = runtimeId,
                                              .tree                 = 3U,
                                              .epoch                = treeEpoch,
                                              .last_source_sequence = 1U,
                                              .dropped              = 4U,
                                              },
    } );
    ASSERT_FALSE( gap.has_value() );
    EXPECT_EQ( gap.error().code, grab::ErrorCode::QueueGap );

    grab::spi::UiDelta delta{
        .runtime          = runtimeId,
        .tree             = 3U,
        .epoch            = treeEpoch,
        .base_revision    = 1U,
        .revision         = 2U,
        .complete         = true,
        .added_nodes      = { node( 2U, grab::role::region, 0U, 2U ) },
        .changed_nodes    = {},
        .removed_nodes    = {},
        .relation_changes = {},
    };
    const auto rejected_delta = store.apply( grab::spi::UiUpdate{
        .source_sequence = 3U,
        .payload         = std::move( delta ),
    } );
    ASSERT_FALSE( rejected_delta.has_value() );
    EXPECT_EQ( rejected_delta.error().code, grab::ErrorCode::ResyncRequired );
    EXPECT_EQ( store.revision(), 1U );

    const auto recovery =
        store.apply( update( 4U, snapshot( 2U, { node( 1U ), node( 2U ) } ) ) );
    ASSERT_TRUE( recovery.has_value() ) << recovery.error().message;
    EXPECT_EQ( store.revision(), 2U );
}

// NOLINTEND(readability-trailing-comma)
