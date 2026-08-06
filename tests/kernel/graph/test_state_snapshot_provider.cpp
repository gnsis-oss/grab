#include "grab/event.hpp"
#include "grab/event_descriptor.hpp"
#include "grab/pid.hpp"
#include "kernel/events/event_bus.hpp"
#include "kernel/graph/state_snapshot_provider.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <atomic>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>
// clang-format on

namespace
{

    constexpr auto   windowCreatedKind    = grab::EventKind::WindowCreated;
    constexpr auto   stateSnapshotKind    = grab::EventKind::StateSnapshot;
    constexpr double timestamp            = 12.5;
    constexpr auto   unsetSequence        = 0U;
    constexpr auto   subscriptionCapacity = 32U;
    constexpr auto   editorPid            = grab::Pid{ 1'001 };
    constexpr auto   contendedPid         = grab::Pid{ 1'002 };
    constexpr int    teardownRounds       = 200;
    constexpr auto   subscriberThreads    = 2U;
    constexpr int    noSubscribes         = 0;

    [[nodiscard]]
    grab::Event
    make_window_event( grab::EventKind kind,
                       const char*     app,
                       grab::Pid       pid,
                       const char*     title )
    {
        return grab::Event{
            .timestamp = timestamp,
            .sequence  = unsetSequence,
            .kind      = kind,
            .category  = grab::category_of( kind ),
            .payload   = grab::Payload{ grab::WindowChange{
                .app        = std::string{ app },
                .pid        = pid,
                .title      = std::string{ title },
                .prev_title = {},
                .duration_s = 0.0,
            } },
        };
    }

    [[nodiscard]]
    std::vector<grab::Event>
    drain( grab::Subscription& subscription )
    {
        std::vector<grab::Event> events;
        while( std::optional<grab::Event> event = subscription.try_pop() )
        {
            events.push_back( *event );
        }
        return events;
    }

}    // namespace

TEST( StateSnapshotProvider,
      ReplaySubscriberGetsStateSnapshotAndWindowCreated )
{
    grab::EventBus                     bus;
    grab::event::StateSnapshotProvider provider{ bus };
    bus.publish(
        make_window_event( windowCreatedKind, "editor", editorPid, "main.cpp" )
    );

    grab::EventFilter filter;
    filter.kinds      = { windowCreatedKind, stateSnapshotKind };
    auto       replay = bus.subscribe( filter, subscriptionCapacity );

    const auto events = drain( replay );

    ASSERT_EQ( events.size(), 2U );

    auto created_count  = 0U;
    auto snapshot_count = 0U;
    for( const auto& event : events )
    {
        if( event.kind == windowCreatedKind )
        {
            const auto* change = std::get_if<grab::WindowChange>( &event.payload );
            ASSERT_NE( change, nullptr );
            EXPECT_EQ( change->app, "editor" );
            ++created_count;
        }
        if( event.kind == stateSnapshotKind )
        {
            const auto* snapshot = std::get_if<grab::StateSnapshot>( &event.payload );
            ASSERT_NE( snapshot, nullptr );
            EXPECT_NE( snapshot->json.find( "editor" ), std::string::npos );
            ++snapshot_count;
        }
    }

    EXPECT_EQ( created_count, 1U );
    EXPECT_EQ( snapshot_count, 1U );
}

TEST( StateSnapshotProvider,
      UnregistersProvidersOnDestruction )
{
    grab::EventBus bus;
    bus.publish(
        make_window_event( windowCreatedKind, "editor", editorPid, "main.cpp" )
    );

    {
        grab::event::StateSnapshotProvider provider{ bus };
    }

    grab::EventFilter filter;
    filter.kinds                  = { stateSnapshotKind };
    auto       replay             = bus.subscribe( filter, subscriptionCapacity );

    const auto events             = drain( replay );
    auto       has_state_snapshot = false;
    for( const auto& event : events )
    {
        if( event.kind == stateSnapshotKind )
        {
            has_state_snapshot = true;
        }
    }

    EXPECT_FALSE( has_state_snapshot );
}

// EventBusState::add invokes a registered snapshot provider WHILE HOLDING the
// bus lock, so the bus takes (bus -> provider). If ~StateSnapshotProvider
// holds the provider lock while ~Subscription unsubscribes, it takes
// (provider -> bus) — an ABBA inversion that ThreadSanitizer reports as a
// potential deadlock.
//
// This drives both orders concurrently. If the inversion is reintroduced the
// two threads deadlock and the test hangs until its CTest timeout, which is
// the intended signal; under TSan it also reports the inversion directly.
TEST( StateSnapshotProvider,
      ConcurrentSubscribeAndTeardownDoNotInvertBusAndProviderLocks )
{
    grab::EventBus bus;
    bus.publish(
        make_window_event( windowCreatedKind, "editor", contendedPid, "main.cpp" )
    );

    std::atomic<bool>        stop{ false };
    std::atomic<int>         subscribes{ 0 };

    std::vector<std::thread> subscribers;
    subscribers.reserve( subscriberThreads );
    for( auto index = 0U; index < subscriberThreads; ++index )
    {
        subscribers.emplace_back(
            [&bus, &stop, &subscribes]
            {
                while( !stop.load( std::memory_order_relaxed ) )
                {
                    grab::EventFilter filter;
                    filter.kinds = { stateSnapshotKind, windowCreatedKind };
                    // Taking (bus -> provider): add() runs the registered
                    // snapshot callback under the bus lock.
                    auto subscription = bus.subscribe( filter, subscriptionCapacity );
                    subscribes.fetch_add( 1, std::memory_order_relaxed );
                }
            }
        );
    }

    for( auto round = 0; round < teardownRounds; ++round )
    {
        // Taking (provider -> bus) if the destructor holds its own lock while
        // the subscription unsubscribes.
        grab::event::StateSnapshotProvider provider{ bus };
    }

    stop.store( true, std::memory_order_relaxed );
    for( auto& thread : subscribers )
    {
        thread.join();
    }

    EXPECT_GT( subscribes.load( std::memory_order_relaxed ), noSubscribes );
}
