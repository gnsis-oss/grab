#include "fake/fake_runtime.hpp"
#include "grab/context.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/ids.hpp"
#include "grab/interaction.hpp"
#include "grab/locator.hpp"
#include "grab/query.hpp"
#include "grab/result.hpp"
#include "grab/role.hpp"
#include "grab/trace.hpp"
#include "grab/ui.hpp"
#include "kernel/lifecycle/session_impl.hpp"
#include "kernel/tree_fixtures.hpp"
#include "spi/route.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <utility>
#include <vector>
// clang-format on

// NOLINTBEGIN(readability-trailing-comma)
namespace
{

    // This matches the primary tree id the session core targets:
    // X11TreeSource::firstTree; SessionCore::attach primes the same id.
    constexpr std::uint32_t        performTree = 1U;
    constexpr grab::NodeId         performNode{ 1U };
    constexpr grab::NodeGeneration performGeneration{ 1U };
    constexpr grab::TreeEpoch      performEpoch{ 1U };
    constexpr std::uint64_t        performRevision = 17U;
    constexpr std::size_t          noRouteCommits  = 0U;
    constexpr std::size_t          oneRouteCommit  = 1U;

    [[nodiscard]]
    grab::UiSnapshot
    perform_snapshot( grab::RuntimeId runtime )
    {
        const auto node = grab::UiNodeRecord{
            performNode,
            performGeneration,
            grab::role::window,
            grab::state_mask( grab::NodeState::Visible ) | grab::NodeState::Enabled,
            {                  },
            grab::UiProvenance{
                                                                                 .runtime  = runtime,.revision = performRevision,
                                                                                 },
        };
        std::vector<grab::UiNodeRecord> nodes{ node };
        std::vector<grab::NodeId>       roots{ node.id };
        return grab::UiSnapshot::from_records(
            grab::UiSnapshotMetadata{
                .runtime  = runtime,
                .tree     = performTree,
                .epoch    = performEpoch,
                .revision = performRevision,
                .complete = true,
            },
            std::move( nodes ),
            std::move( roots ),
            {}
        );
    }

}    // namespace

TEST( SessionVerbs,
      ResolveFindsNodeAndWatchYieldsSubscriptionId )
{
    grab::testing::FakeRuntime fake;
    fake.inject_snapshot( grab::testing::tree::snapshot(
        1U,
        { grab::testing::tree::node( 1U, grab::role::window ) }
    ) );

    auto core = grab::kernel::lifecycle::SessionCore::open_for_test();
    ASSERT_NE( core, nullptr );
    const grab::OperationContext context{};
    ASSERT_TRUE( core->attach( fake, context ).has_value() );

    const auto match = core->resolve( grab::sel::role( grab::role::window ),
                                      grab::Cardinality::ExactlyOne );
    ASSERT_TRUE( match.has_value() );
    EXPECT_EQ( match->ref.node, 1U );

    auto sub = core->watch( grab::SubscriptionScope{
        .kinds  = { grab::EventKind::NodeChanged },
        .filter = {},
    } );
    ASSERT_TRUE( sub.has_value() );
    EXPECT_NE( sub->id(), grab::SubscriptionId{} );
}

TEST( SessionVerbs,
      ResolveExactlyOneOnEmptyScopeReturnsNoMatch )
{
    grab::testing::FakeRuntime fake;
    fake.inject_snapshot( grab::testing::tree::snapshot(
        1U,
        { grab::testing::tree::node( 1U, grab::role::window ) }
    ) );

    auto core = grab::kernel::lifecycle::SessionCore::open_for_test();
    ASSERT_NE( core, nullptr );
    const grab::OperationContext context{};
    ASSERT_TRUE( core->attach( fake, context ).has_value() );

    const auto match = core->resolve( grab::sel::role( grab::role::button ),
                                      grab::Cardinality::ExactlyOne );
    ASSERT_FALSE( match.has_value() );
    EXPECT_EQ( match.error().code, grab::ErrorCode::NoMatch );
}

TEST( SessionVerbs,
      PerformClickReturnsReceiptWithCommitStatus )
{
    grab::testing::FakeRuntime fake;
    fake.inject_snapshot( perform_snapshot( fake.runtime_id() ) );
    auto& route = fake.add_route( "pointer.click", grab::spi::RouteKind::Physical );

    auto  core  = grab::kernel::lifecycle::SessionCore::open_for_test();
    ASSERT_NE( core, nullptr );
    const grab::OperationContext context{};
    ASSERT_TRUE( core->attach( fake, context ).has_value() );

    // Actionability's state_stable( 2U ) predicate needs a second observation;
    // one no-op wakeup lets the WaitEngine re-observe.
    fake.event_source()->push_wakeup(
        []
        {
        }
    );
    const auto receipt =
        core->perform( grab::Click{ .target = grab::sel::role( grab::role::window ) },
                       {} );

    ASSERT_TRUE( receipt.has_value() );
    // Fully successful transactions end Verified; Committed-only outcomes
    // always carry an error.
    EXPECT_EQ( receipt->commit, grab::CommitStatus::Verified );
    EXPECT_FALSE( receipt->routes.empty() );
    EXPECT_EQ( route.commit_count(), oneRouteCommit );
}

TEST( SessionVerbs,
      PerformHonorsCallerStopToken )
{
    grab::testing::FakeRuntime fake;
    fake.inject_snapshot( perform_snapshot( fake.runtime_id() ) );
    auto& route = fake.add_route( "pointer.click", grab::spi::RouteKind::Physical );

    auto  core  = grab::kernel::lifecycle::SessionCore::open_for_test();
    ASSERT_NE( core, nullptr );
    const grab::OperationContext context{};
    ASSERT_TRUE( core->attach( fake, context ).has_value() );

    std::stop_source stop;
    stop.request_stop();
    const auto receipt =
        core->perform( grab::Click{ .target = grab::sel::role( grab::role::window ) },
                       grab::ActionOptions{ .stop = stop.get_token() } );

    ASSERT_FALSE( receipt.has_value() );
    EXPECT_EQ( receipt.error().code, grab::ErrorCode::Cancelled );
    EXPECT_EQ( route.commit_count(), noRouteCommits );
}

// NOLINTEND(readability-trailing-comma)
