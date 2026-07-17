#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/event_descriptor.hpp"
#include "grab/pid.hpp"
#include "kernel/graph/state_snapshot_provider.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <optional>
#include <string>
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
        make_window_event( windowCreatedKind, "editor", grab::Pid{ 1'001 }, "main.cpp" )
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
        make_window_event( windowCreatedKind, "editor", grab::Pid{ 1'001 }, "main.cpp" )
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
