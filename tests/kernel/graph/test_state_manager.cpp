#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/event_descriptor.hpp"
#include "grab/pid.hpp"
#include "kernel/graph/state_manager.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
// clang-format on

namespace
{

    constexpr auto          windowCreatedKind      = grab::EventKind::WindowCreated;
    constexpr auto          windowClosedKind       = grab::EventKind::WindowClosed;
    constexpr auto          windowFocusChangedKind = grab::EventKind::WindowFocusChanged;
    constexpr auto          keyDownKind            = grab::EventKind::KeyDown;
    constexpr auto          stateSnapshotKind      = grab::EventKind::StateSnapshot;
    constexpr auto          stateCategory          = grab::EventCategory::State;
    constexpr double        timestamp              = 12.5;
    constexpr std::uint64_t unsetSequence          = 0U;
    constexpr std::uint32_t keyCode                = 9U;
    constexpr std::size_t   noOpenWindows          = 0U;
    constexpr std::size_t   oneOpenWindow          = 1U;
    constexpr std::size_t   twoOpenWindows         = 2U;
    constexpr std::string_view firstApp            = "editor";
    constexpr std::int64_t     firstPidValue       = 1'001;
    constexpr grab::Pid        firstPid{ firstPidValue };
    constexpr std::string_view firstTitle     = "main.cpp";
    constexpr std::string_view secondApp      = "browser";
    constexpr std::int64_t     secondPidValue = 1'002;
    constexpr grab::Pid        secondPid{ secondPidValue };
    constexpr std::string_view secondTitle = "Release notes";
    constexpr std::string_view keyName     = "tab";
    constexpr std::string_view openKey     = R"("open")";
    constexpr std::string_view focusedKey  = R"("focused")";

    [[nodiscard]]
    grab::Event
    make_window_event( grab::EventKind  kind,
                       std::string_view app,
                       grab::Pid        pid,
                       std::string_view title )
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
    grab::Event
    make_key_down_event()
    {
        return grab::Event{
            .timestamp = timestamp,
            .sequence  = unsetSequence,
            .kind      = keyDownKind,
            .category  = grab::category_of( keyDownKind ),
            .payload   = grab::Payload{ grab::InputKey{
                .code = keyCode,
                .name = std::string{ keyName },
            } },
        };
    }

    [[nodiscard]]
    const grab::StateSnapshot*
    state_snapshot_payload( const grab::Event& event )
    {
        return std::get_if<grab::StateSnapshot>( &event.payload );
    }

}    // namespace

TEST( StateManager,
      TracksOpenWindows )
{
    grab::event::StateManager state;

    state.observe(
        make_window_event( windowCreatedKind, firstApp, firstPid, firstTitle )
    );
    state.observe(
        make_window_event( windowCreatedKind, secondApp, secondPid, secondTitle )
    );

    EXPECT_EQ( state.open_window_count(), twoOpenWindows );

    state.observe(
        make_window_event( windowClosedKind, firstApp, firstPid, firstTitle )
    );

    EXPECT_EQ( state.open_window_count(), oneOpenWindow );
}

TEST( StateManager,
      SnapshotReflectsFocused )
{
    grab::event::StateManager state;

    state.observe(
        make_window_event( windowCreatedKind, firstApp, firstPid, firstTitle )
    );
    state.observe(
        make_window_event( windowFocusChangedKind, firstApp, firstPid, firstTitle )
    );

    const auto  snapshot = state.snapshot( timestamp );
    const auto* payload  = state_snapshot_payload( snapshot );

    ASSERT_NE( payload, nullptr );
    EXPECT_NE( payload->json.find( focusedKey ), std::string::npos );
    EXPECT_NE( payload->json.find( firstTitle ), std::string::npos );
}

TEST( StateManager,
      OpenWindowEventsReplayCreatedWindows )
{
    grab::event::StateManager state;

    state.observe(
        make_window_event( windowCreatedKind, firstApp, firstPid, firstTitle )
    );
    state.observe(
        make_window_event( windowCreatedKind, secondApp, secondPid, secondTitle )
    );

    const auto events = state.open_window_events( timestamp );

    ASSERT_EQ( events.size(), twoOpenWindows );
    EXPECT_EQ( events.at( 0U ).kind, windowCreatedKind );
    EXPECT_EQ( events.at( 1U ).kind, windowCreatedKind );

    const auto* first_change =
        std::get_if<grab::WindowChange>( &events.at( 0U ).payload );
    const auto* second_change =
        std::get_if<grab::WindowChange>( &events.at( 1U ).payload );

    ASSERT_NE( first_change, nullptr );
    ASSERT_NE( second_change, nullptr );
    EXPECT_EQ( first_change->app, firstApp );
    EXPECT_EQ( first_change->title, firstTitle );
    EXPECT_EQ( second_change->app, secondApp );
    EXPECT_EQ( second_change->title, secondTitle );
}

TEST( StateManager,
      SnapshotIsStateKind )
{
    const grab::event::StateManager state;

    const auto                      snapshot = state.snapshot( timestamp );

    EXPECT_EQ( snapshot.timestamp, timestamp );
    EXPECT_EQ( snapshot.sequence, unsetSequence );
    EXPECT_EQ( snapshot.kind, stateSnapshotKind );
    EXPECT_EQ( snapshot.category, stateCategory );
}

TEST( StateManager,
      IgnoresNonWindowEvents )
{
    grab::event::StateManager state;

    state.observe( make_key_down_event() );

    EXPECT_EQ( state.open_window_count(), noOpenWindows );
}

TEST( StateManager,
      PublishSnapshotEmitsOnBus )
{
    grab::EventBus                  bus;
    auto                            subscription = bus.subscribe( grab::EventFilter{
        .kinds      = { stateSnapshotKind },
        .categories = {},
    } );
    const grab::event::StateManager state;

    state.publish_snapshot( bus, timestamp );

    const std::optional<grab::Event> event = subscription.try_pop();
    ASSERT_TRUE( event.has_value() );
    EXPECT_EQ( event->kind, stateSnapshotKind );

    const auto* payload = state_snapshot_payload( *event );
    ASSERT_NE( payload, nullptr );
    EXPECT_NE( payload->json.find( openKey ), std::string::npos );
}
