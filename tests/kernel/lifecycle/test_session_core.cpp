#include "fake/fake_runtime.hpp"
#include "grab/context.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/ids.hpp"
#include "grab/relation.hpp"
#include "grab/role.hpp"
#include "grab/ui.hpp"
#include "kernel/lifecycle/session_impl.hpp"
#include "kernel/tree_fixtures.hpp"
#include "spi/tree_source.hpp"

// clang-format off
#include <gtest/gtest.h>
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

// NOLINTEND(readability-trailing-comma)
