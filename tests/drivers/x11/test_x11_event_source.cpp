#include "drivers/desktop/x11/x11_event_source.hpp"
#include "drivers/desktop/x11/x11_routes.hpp"
#include "drivers/desktop/x11/x11_runtime.hpp"
#include "drivers/desktop/x11/x11_topology_source.hpp"
#include "grab/context.hpp"
#include "grab/event.hpp"
#include "grab/origin.hpp"
#include "grab/session.hpp"
#include "kernel/events/event_bus.hpp"
#include "kernel/lifecycle/session_impl.hpp"
#include "spi/event_source.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
// clang-format on

namespace
{

    constexpr std::uint8_t  test_keycode            = 38U;
    constexpr std::uint8_t  test_button             = 1U;
    constexpr std::size_t   no_subscriptions        = 0U;
    constexpr std::size_t   one_subscription        = 1U;
    constexpr std::size_t   maximum_pump_iterations = 10U;
    constexpr std::uint64_t initial_generation      = 0U;
    constexpr std::size_t   no_refreshes            = 0U;
    constexpr auto          event_wait_budget       = std::chrono::seconds{ 5 };
    constexpr auto          short_context_budget    = std::chrono::milliseconds{ 400 };
    constexpr auto          short_wait_budget       = std::chrono::milliseconds{ 250 };

}    // namespace

// NOLINTBEGIN(readability-trailing-comma)

// GoogleTest assertion macros inflate the reported cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST( X11EventSource,
      EnableInjectWaitYieldsInjectedSelfKeyDown )
{
    const char* const display = std::getenv( "DISPLAY" );
    if( display == nullptr || std::string_view{ display }.empty() )
    {
        GTEST_SKIP() << "requires Xvfb (DISPLAY is not set)";
    }

    grab::drivers::desktop::x11::X11Runtime runtime;
    const grab::OperationContext            start_context{
        .deadline = grab::Deadline::unbounded(),
    };
    ASSERT_TRUE( runtime.start( start_context ).has_value() );

    std::vector<grab::Event> events;
    runtime.set_event_sink(
        [&events]( grab::Event&& event )
        {
            events.push_back( std::move( event ) );
        }
    );

    auto* const event_source = runtime.event_source();
    ASSERT_NE( event_source, nullptr );
    const grab::spi::EventSpec spec{ "input.key_down" };
    ASSERT_TRUE( event_source->enable( spec ).has_value() );

    auto* const seat = runtime.native_seat();
    ASSERT_NE( seat, nullptr );
    ASSERT_TRUE( seat->key( test_keycode, true ).has_value() );
    ASSERT_TRUE( seat->key( test_keycode, false ).has_value() );
    ASSERT_TRUE( seat->flush().has_value() );

    const grab::OperationContext wait_context{
        .deadline = grab::Deadline::after( event_wait_budget ),
    };
    ASSERT_TRUE(
        event_source->wait_for_event( spec, wait_context, event_wait_budget ).has_value()
    );

    const auto injected_key_down = std::ranges::find_if(
        events,
        []( const grab::Event& event )
        {
            return event.kind ==
                   grab::EventKind::KeyDown &&
                   event.origin ==
                   grab::EventOrigin::InjectedSelf &&
                   std::get<grab::InputKey>( event.payload ).code == test_keycode;
        }
    );
    ASSERT_NE( injected_key_down, events.end() );
    EXPECT_TRUE( std::ranges::none_of( events,
                                       []( const grab::Event& event )
                                       {
                                           return event.origin ==
                                                  grab::EventOrigin::Unknown;
                                       } ) );

    ASSERT_TRUE( runtime.stop().has_value() );
}

// GoogleTest assertion macros inflate the reported cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST( X11EventSource,
      DemandSubscriptionTogglesXi2MaskDelivery )
{
    const char* const display = std::getenv( "DISPLAY" );
    if( display == nullptr || std::string_view{ display }.empty() )
    {
        GTEST_SKIP() << "requires Xvfb (DISPLAY is not set)";
    }

    auto opened_core =
        grab::kernel::lifecycle::SessionCore::open( grab::SessionOptions{}, nullptr );
    ASSERT_TRUE( opened_core.has_value() );
    auto core = std::move( *opened_core );
    ASSERT_NE( core, nullptr );

    EXPECT_EQ( core->bus().subscription_refcount( grab::EventKind::MouseClick ),
               no_subscriptions );

    std::optional<grab::Subscription> subscription;
    auto                              watched = core->watch( grab::SubscriptionScope{
        .kinds  = { grab::EventKind::MouseClick },
        .filter = {},
    } );
    ASSERT_TRUE( watched.has_value() );
    subscription.emplace( std::move( *watched ) );
    EXPECT_EQ( core->bus().subscription_refcount( grab::EventKind::MouseClick ),
               one_subscription );

    auto* const x11 = dynamic_cast<grab::drivers::desktop::x11::X11Runtime*>(
        &core->primary_runtime()
    );
    ASSERT_NE( x11, nullptr );
    auto* const seat = x11->native_seat();
    ASSERT_NE( seat, nullptr );

    ASSERT_TRUE( seat->button( test_button, true ).has_value() );
    ASSERT_TRUE( seat->button( test_button, false ).has_value() );
    ASSERT_TRUE( seat->flush().has_value() );

    auto* const event_source = x11->event_source();
    ASSERT_NE( event_source, nullptr );
    const grab::spi::EventSpec   spec{ "input.mouse_click" };
    const grab::OperationContext pump_context{
        .deadline = grab::Deadline::after( event_wait_budget ),
    };
    std::optional<grab::Event> delivered;
    for( std::size_t iteration{};
         iteration < maximum_pump_iterations && !delivered.has_value();
         ++iteration )
    {
        ASSERT_TRUE( event_source
                         ->wait_for_event( spec, pump_context, event_wait_budget )
                         .has_value() );
        delivered = subscription->try_pop();
    }

    ASSERT_TRUE( delivered.has_value() );
    EXPECT_EQ( delivered->kind, grab::EventKind::MouseClick );
    EXPECT_EQ( delivered->origin, grab::EventOrigin::InjectedSelf );

    while( subscription->try_pop().has_value() )
    {
    }
    subscription.reset();
    EXPECT_EQ( core->bus().subscription_refcount( grab::EventKind::MouseClick ),
               no_subscriptions );

    ASSERT_TRUE( seat->button( test_button, true ).has_value() );
    ASSERT_TRUE( seat->button( test_button, false ).has_value() );
    ASSERT_TRUE( seat->flush().has_value() );

    auto watched_again = core->watch( grab::SubscriptionScope{
        .kinds  = { grab::EventKind::MouseClick },
        .filter = {},
    } );
    ASSERT_TRUE( watched_again.has_value() );
    auto                         second_subscription = std::move( *watched_again );

    const grab::OperationContext short_context{
        .deadline = grab::Deadline::after( short_context_budget ),
    };
    static_cast<void>(
        event_source->wait_for_event( spec, short_context, short_wait_budget )
    );
    EXPECT_FALSE( second_subscription.try_pop().has_value() );
}

TEST( X11TopologySource,
      PollReturnsOutputsWithStableBaseline )
{
    const char* const display = std::getenv( "DISPLAY" );
    if( display == nullptr || std::string_view{ display }.empty() )
    {
        GTEST_SKIP() << "requires Xvfb (DISPLAY is not set)";
    }

    std::size_t                                    refresh_count{};
    grab::drivers::desktop::x11::X11TopologySource source{
        [&refresh_count]
        {
            ++refresh_count;
        },
    };

    const auto first = source.poll();
    ASSERT_TRUE( first.has_value() );
    EXPECT_FALSE( first->outputs.empty() );
    EXPECT_FALSE( first->changed );
    EXPECT_EQ( first->generation, initial_generation );

    const auto second = source.poll();
    ASSERT_TRUE( second.has_value() );
    EXPECT_FALSE( second->changed );
    EXPECT_EQ( second->generation, first->generation );
    EXPECT_EQ( second->outputs.size(), first->outputs.size() );
    EXPECT_EQ( refresh_count, no_refreshes );
}

// NOLINTEND(readability-trailing-comma)
