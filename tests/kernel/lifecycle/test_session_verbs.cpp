#include "fake/fake_runtime.hpp"
#include "grab/context.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/ids.hpp"
#include "grab/locator.hpp"
#include "grab/query.hpp"
#include "grab/result.hpp"
#include "grab/role.hpp"
#include "kernel/lifecycle/session_impl.hpp"
#include "kernel/tree_fixtures.hpp"

// clang-format off
#include <gtest/gtest.h>
// clang-format on

// NOLINTBEGIN(readability-trailing-comma)
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

// NOLINTEND(readability-trailing-comma)
