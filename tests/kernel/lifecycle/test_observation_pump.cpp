#include "fake/fake_runtime.hpp"
#include "grab/context.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/event_descriptor.hpp"
#include "grab/origin.hpp"
#include "grab/role.hpp"
#include "kernel/lifecycle/session_impl.hpp"
#include "kernel/tree_fixtures.hpp"
#include "spi/event_source.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <chrono>
#include <optional>
#include <thread>
// clang-format on

namespace
{

    constexpr auto pollInterval = std::chrono::milliseconds{ 5 };
    constexpr auto waitBudget   = std::chrono::seconds{ 2 };

    [[nodiscard]]
    std::optional<grab::Event>
    poll_for_event( grab::Subscription& subscription )
    {
        const auto deadline = std::chrono::steady_clock::now() + waitBudget;
        while( std::chrono::steady_clock::now() < deadline )
        {
            auto event = subscription.try_pop();
            if( event.has_value() )
            {
                return event;
            }
            std::this_thread::sleep_for( pollInterval );
        }
        return std::nullopt;
    }

}    // namespace

TEST( ObservationPump,
      PumpedEventSourceEventReachesBusWithOrigin )
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

    auto watch = core->bus().subscribe( grab::SubscriptionScope{
        .kinds  = { grab::EventKind::KeyDown },
        .filter = {},
    } );

    ASSERT_TRUE( core->start_observation( context ).has_value() );

    grab::Event scripted{};
    scripted.kind     = grab::EventKind::KeyDown;
    scripted.category = grab::EventCategory::Input;
    scripted.origin   = grab::EventOrigin::Physical;
    fake.event_source()->push_event( scripted );

    const auto received = poll_for_event( watch );
    core->stop_observation();

    ASSERT_TRUE( received.has_value() );
    EXPECT_EQ( received->kind, grab::EventKind::KeyDown );
    EXPECT_EQ( received->origin, grab::EventOrigin::Physical );
}

TEST( ObservationPump,
      SubscriptionTogglesEventSourceDemand )
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
    ASSERT_TRUE( core->start_observation( context ).has_value() );

    const grab::spi::EventSpec key_spec{
        .name = std::string{ grab::wire_name( grab::EventKind::KeyDown ) },
    };
    EXPECT_EQ( fake.event_source()->demand_count( key_spec ), 0U );

    {
        auto sub = core->bus().subscribe( grab::SubscriptionScope{
            .kinds  = { grab::EventKind::KeyDown },
            .filter = {},    // NOLINT(readability-trailing-comma)
        } );
        EXPECT_EQ( fake.event_source()->demand_count( key_spec ), 1U );
    }
    EXPECT_EQ( fake.event_source()->demand_count( key_spec ), 0U );

    core->stop_observation();
}
