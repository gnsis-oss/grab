#include "fake/fake_runtime.hpp"
#include "grab/context.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/ids.hpp"
#include "grab/locator.hpp"
#include "grab/query.hpp"
#include "grab/relation.hpp"
#include "grab/role.hpp"
#include "grab/ui.hpp"
#include "kernel/lifecycle/session_impl.hpp"
#include "kernel/tree_fixtures.hpp"
#include "spi/tree_source.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstddef>
#include <cstdint>
#include <variant>
// clang-format on

// NOLINTBEGIN(readability-trailing-comma)
TEST( SessionCore,
      AttachedFakeRuntimeSnapshotReachesStoreAndBus )
{
    grab::testing::FakeRuntime fake;
    fake.inject_snapshot( grab::testing::tree::snapshot(
        1U,
        { grab::testing::tree::node( 1U, grab::role::window ) }
    ) );

    auto core = grab::kernel::lifecycle::SessionCore::open_for_test();
    ASSERT_NE( core, nullptr );

    auto                         watch = core->bus().subscribe( grab::SubscriptionScope{
        .kinds  = { grab::EventKind::NodeAdded },
        .filter = {},
    } );

    const grab::OperationContext context{};
    ASSERT_TRUE( core->attach( fake, context ).has_value() );

    const auto event = watch.try_pop();
    ASSERT_TRUE( event.has_value() );
    EXPECT_EQ( event->kind, grab::EventKind::NodeAdded );
    EXPECT_EQ( event->category, grab::EventCategory::Window );
    EXPECT_EQ( event->after_revision, core->store().revision() );
    EXPECT_EQ( event->before_revision, 0U );
    ASSERT_TRUE( event->subject.has_value() );
    EXPECT_EQ( event->subject->node, 1U );
    const auto* change = std::get_if<grab::GraphChange>( &event->payload );
    ASSERT_NE( change, nullptr );
    EXPECT_EQ( change->node, 1U );
}

TEST( SessionCore,
      ActiveChildRelationDeltaEmitsActiveChildChanged )
{
    grab::testing::FakeRuntime fake;
    fake.inject_snapshot( grab::testing::tree::snapshot(
        1U,
        {
            grab::testing::tree::node( 1U, grab::role::application ),
            grab::testing::tree::node( 2U, grab::role::window ),
            grab::testing::tree::node( 3U, grab::role::window ),
    },
        {
            grab::UiRelation{
                .source   = grab::NodeId{ 1U },
                .target   = grab::NodeId{ 2U },
                .relation = grab::relation::contains,
            },
            grab::UiRelation{
                .source   = grab::NodeId{ 1U },
                .target   = grab::NodeId{ 3U },
                .relation = grab::relation::contains,
            },
            grab::UiRelation{
                .source   = grab::NodeId{ 1U },
                .target   = grab::NodeId{ 2U },
                .relation = grab::relation::active_child,
            },
        }
    ) );

    auto core = grab::kernel::lifecycle::SessionCore::open_for_test();
    ASSERT_NE( core, nullptr );
    const grab::OperationContext context{};
    ASSERT_TRUE( core->attach( fake, context ).has_value() );

    // Subscribe after attach so only the delta's synthesized event is seen.
    auto watch = core->bus().subscribe( grab::SubscriptionScope{
        .kinds  = { grab::EventKind::ActiveChildChanged },
        .filter = {},
    } );

    fake.inject_delta( grab::spi::UiDelta{
        .runtime          = grab::testing::tree::fixtureRuntime,
        .tree             = grab::testing::tree::fixtureTree,
        .epoch            = grab::testing::tree::fixtureEpoch,
        .base_revision    = 1U,
        .revision         = 2U,
        .complete         = true,
        .added_nodes      = {},
        .changed_nodes    = {},
        .removed_nodes    = {                 },
        .relation_changes = {
                             grab::spi::RelationChange{
                .kind     = grab::spi::RelationChangeKind::Remove,
                .source   = grab::NodeId{ 1U },
                .target   = grab::NodeId{ 2U },
                .relation = grab::relation::active_child,
            },grab::spi::RelationChange{
                .kind     = grab::spi::RelationChangeKind::Add,
                .source   = grab::NodeId{ 1U },
                .target   = grab::NodeId{ 3U },
                .relation = grab::relation::active_child,
            }, },
    } );
    ASSERT_TRUE( core->pump_once( context ).has_value() );

    const auto event = watch.try_pop();
    ASSERT_TRUE( event.has_value() );
    EXPECT_EQ( event->kind, grab::EventKind::ActiveChildChanged );
    EXPECT_EQ( event->category, grab::EventCategory::Window );
    EXPECT_EQ( event->before_revision, 1U );
    EXPECT_EQ( event->after_revision, 2U );
    const auto* change = std::get_if<grab::GraphChange>( &event->payload );
    ASSERT_NE( change, nullptr );
    EXPECT_EQ( change->node, 1U );
    EXPECT_EQ( change->related, 3U );
    EXPECT_EQ( change->relation, grab::relation::active_child.value );
    EXPECT_EQ( change->previous_active, 2U );
    EXPECT_FALSE( watch.try_pop().has_value() );
}

TEST( SessionCore,
      OpenForTestRecordsNoRuntimeDiagnostics )
{
    auto core = grab::kernel::lifecycle::SessionCore::open_for_test();
    ASSERT_NE( core, nullptr );
    EXPECT_TRUE( core->runtime_diagnostics().empty() );
}

TEST( SessionCore,
      SessionAssignsDistinctSubjectRuntimeIdsAcrossAttachedRuntimes )
{
    grab::testing::FakeRuntime fake_a;
    grab::testing::FakeRuntime fake_b;
    fake_a.inject_snapshot( grab::testing::tree::snapshot(
        1U,
        { grab::testing::tree::node( 1U, grab::role::window ) }
    ) );
    fake_b.inject_snapshot( grab::testing::tree::snapshot(
        1U,
        { grab::testing::tree::node( 2U, grab::role::window ) }
    ) );

    auto core = grab::kernel::lifecycle::SessionCore::open_for_test();
    ASSERT_NE( core, nullptr );

    auto                         watch = core->bus().subscribe( grab::SubscriptionScope{
        .kinds  = { grab::EventKind::NodeAdded },
        .filter = {},
    } );

    const grab::OperationContext context{};
    ASSERT_TRUE( core->attach( fake_a, context ).has_value() );
    ASSERT_TRUE( core->attach( fake_b, context ).has_value() );

    // Both fakes stamp their snapshots with the same SOURCE runtime id
    // (fixtureRuntime). The session must still assign each attached runtime a
    // distinct authority id, surfaced as a distinct subject.runtime.
    const auto event_a = watch.try_pop();
    ASSERT_TRUE( event_a.has_value() );
    ASSERT_TRUE( event_a->subject.has_value() );

    const auto event_b = watch.try_pop();
    ASSERT_TRUE( event_b.has_value() );
    ASSERT_TRUE( event_b->subject.has_value() );

    const auto id_a = core->runtime_id_at( 0U );
    const auto id_b = core->runtime_id_at( 1U );
    EXPECT_NE( id_a, id_b );
    EXPECT_EQ( event_a->subject->runtime, id_a );
    EXPECT_EQ( event_b->subject->runtime, id_b );
    EXPECT_NE( event_a->subject->runtime, event_b->subject->runtime );

    // A runtime restart that re-attaches the same runtime object keeps its
    // session id; it is not re-minted into a colliding value.
    fake_a.restart();
    ASSERT_TRUE( core->attach( fake_a, context ).has_value() );
    EXPECT_EQ( core->runtime_id_at( 0U ), id_a );
    EXPECT_NE( core->runtime_id_at( 0U ), core->runtime_id_at( 1U ) );
}

TEST( SessionCore,
      TwoAttachedRuntimesKeepIndependentStoresAndShareOneBus )
{
    constexpr grab::RuntimeId      secondRuntime{ 9U };
    constexpr std::uint32_t        secondTree = 1U;
    constexpr grab::TreeEpoch      secondEpoch{ 1U };
    constexpr std::uint64_t        initialRevision = 1U;
    constexpr std::uint64_t        updatedRevision = 2U;
    constexpr grab::NodeId         firstNode{ 1U };
    constexpr grab::NodeId         secondNode{ 2U };
    constexpr grab::NodeGeneration nodeGeneration{ 1U };
    constexpr std::uint32_t        noStates = 0U;
    constexpr std::uint32_t visibleState = grab::state_mask( grab::NodeState::Visible );
    constexpr std::size_t   primaryStoreIndex    = 0U;
    constexpr std::size_t   secondaryStoreIndex  = 1U;
    constexpr std::size_t   outOfRangeStoreIndex = 2U;
    constexpr std::size_t   attachedStoreCount   = 2U;

    grab::testing::FakeRuntime fake_a;
    grab::testing::FakeRuntime fake_b;
    fake_a.inject_snapshot( grab::testing::tree::snapshot(
        initialRevision,
        { grab::testing::tree::node( firstNode.value, grab::role::window ) }
    ) );
    fake_b.inject_snapshot( grab::UiSnapshot::from_records(
        grab::UiSnapshotMetadata{
            .runtime  = secondRuntime,
            .tree     = secondTree,
            .epoch    = secondEpoch,
            .revision = initialRevision,
            .complete = true,
    },
        {
            grab::UiNodeRecord{
                secondNode,
                nodeGeneration,
                grab::role::button,
                noStates,
                {},
                grab::UiProvenance{
                    .runtime  = secondRuntime,
                    .revision = initialRevision,
                },
            },
        },
        { secondNode },
        {}
    ) );

    auto core = grab::kernel::lifecycle::SessionCore::open_for_test();
    ASSERT_NE( core, nullptr );

    auto                         watch = core->bus().subscribe( grab::SubscriptionScope{
        .kinds  = { grab::EventKind::NodeAdded },
        .filter = {},
    } );

    const grab::OperationContext context{};
    ASSERT_TRUE( core->attach( fake_a, context ).has_value() );
    ASSERT_TRUE( core->attach( fake_b, context ).has_value() );

    EXPECT_EQ( core->store_count(), attachedStoreCount );

    const auto primary_snapshot = core->store().snapshot();
    ASSERT_TRUE( primary_snapshot.has_value() );
    EXPECT_EQ( primary_snapshot->runtime, grab::testing::tree::fixtureRuntime );

    EXPECT_EQ( core->store_at( primaryStoreIndex ), &core->store() );
    auto* const secondary_store = core->store_at( secondaryStoreIndex );
    ASSERT_NE( secondary_store, nullptr );
    const auto secondary_snapshot = secondary_store->snapshot();
    ASSERT_TRUE( secondary_snapshot.has_value() );
    EXPECT_EQ( secondary_snapshot->runtime, secondRuntime );
    EXPECT_EQ( core->store_at( outOfRangeStoreIndex ), nullptr );

    const auto first_event = watch.try_pop();
    ASSERT_TRUE( first_event.has_value() );
    ASSERT_TRUE( first_event->subject.has_value() );
    EXPECT_EQ( first_event->subject->runtime, core->runtime_id_at( 0U ) );

    const auto second_event = watch.try_pop();
    ASSERT_TRUE( second_event.has_value() );
    ASSERT_TRUE( second_event->subject.has_value() );
    EXPECT_EQ( second_event->subject->runtime, core->runtime_id_at( 1U ) );
    EXPECT_NE( first_event->subject->runtime, second_event->subject->runtime );
    EXPECT_FALSE( watch.try_pop().has_value() );

    const auto primary_match = core->resolve( grab::sel::role( grab::role::window ) );
    ASSERT_TRUE( primary_match.has_value() );
    EXPECT_EQ( primary_match->ref.node, firstNode.value );

    const auto secondary_match = core->resolve( grab::sel::role( grab::role::button ) );
    EXPECT_FALSE( secondary_match.has_value() );

    fake_b.inject_delta( grab::spi::UiDelta{
        .runtime       = secondRuntime,
        .tree          = secondTree,
        .epoch         = secondEpoch,
        .base_revision = initialRevision,
        .revision      = updatedRevision,
        .complete      = true,
        .added_nodes   = {},
        .changed_nodes =
            {
                          grab::UiNodeRecord{
                    secondNode,
                    nodeGeneration,
                    grab::role::button,
                    visibleState,
                    {},
                    grab::UiProvenance{
                        .runtime  = secondRuntime,
                        .revision = updatedRevision,
                    },
                }, },
        .removed_nodes    = {},
        .relation_changes = {},
    } );
    ASSERT_TRUE( core->pump_once( context ).has_value() );

    EXPECT_EQ( secondary_store->revision(), updatedRevision );
    EXPECT_EQ( core->store().revision(), initialRevision );
}

// NOLINTEND(readability-trailing-comma)
