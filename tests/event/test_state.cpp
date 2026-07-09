#include "event/state.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"

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

    constexpr auto   kWindowCreatedKind        = grab::EventKind::window_created;
    constexpr auto   kWindowClosedKind         = grab::EventKind::window_closed;
    constexpr auto   kWindowFocusChangedKind   = grab::EventKind::window_focus_changed;
    constexpr auto   kKeyDownKind              = grab::EventKind::key_down;
    constexpr auto   kStateSnapshotKind        = grab::EventKind::state_snapshot;
    constexpr auto   kStateCategory            = grab::EventCategory::state;
    constexpr double kTimestamp                = 12.5;
    constexpr std::uint64_t    kUnsetSequence  = 0U;
    constexpr std::uint32_t    kKeyCode        = 9U;
    constexpr std::size_t      kNoOpenWindows  = 0U;
    constexpr std::size_t      kOneOpenWindow  = 1U;
    constexpr std::size_t      kTwoOpenWindows = 2U;
    constexpr std::string_view kFirstApp       = "editor";
    constexpr std::string_view kFirstPid       = "1001";
    constexpr std::string_view kFirstTitle     = "main.cpp";
    constexpr std::string_view kSecondApp      = "browser";
    constexpr std::string_view kSecondPid      = "1002";
    constexpr std::string_view kSecondTitle    = "Release notes";
    constexpr std::string_view kKeyName        = "tab";
    constexpr std::string_view kOpenKey        = R"("open")";
    constexpr std::string_view kFocusedKey     = R"("focused")";

    [[nodiscard]]
    grab::Event
    make_window_event( grab::EventKind  kind,
                       std::string_view app,
                       std::string_view pid,
                       std::string_view title )
    {
        return grab::Event{
            .timestamp = kTimestamp,
            .sequence  = kUnsetSequence,
            .kind      = kind,
            .category  = grab::category_of( kind ),
            .payload   = grab::Payload{ grab::WindowChange{
                .app        = std::string{ app },
                .pid        = std::string{ pid },
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
            .timestamp = kTimestamp,
            .sequence  = kUnsetSequence,
            .kind      = kKeyDownKind,
            .category  = grab::category_of( kKeyDownKind ),
            .payload   = grab::Payload{ grab::InputKey{
                .code = kKeyCode,
                .name = std::string{ kKeyName },
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
        make_window_event( kWindowCreatedKind, kFirstApp, kFirstPid, kFirstTitle )
    );
    state.observe(
        make_window_event( kWindowCreatedKind, kSecondApp, kSecondPid, kSecondTitle )
    );

    EXPECT_EQ( state.open_window_count(), kTwoOpenWindows );

    state.observe(
        make_window_event( kWindowClosedKind, kFirstApp, kFirstPid, kFirstTitle )
    );

    EXPECT_EQ( state.open_window_count(), kOneOpenWindow );
}

TEST( StateManager,
      SnapshotReflectsFocused )
{
    grab::event::StateManager state;

    state.observe(
        make_window_event( kWindowCreatedKind, kFirstApp, kFirstPid, kFirstTitle )
    );
    state.observe(
        make_window_event( kWindowFocusChangedKind, kFirstApp, kFirstPid, kFirstTitle )
    );

    const auto  snapshot = state.snapshot( kTimestamp );
    const auto* payload  = state_snapshot_payload( snapshot );

    ASSERT_NE( payload, nullptr );
    EXPECT_NE( payload->json.find( kFocusedKey ), std::string::npos );
    EXPECT_NE( payload->json.find( kFirstTitle ), std::string::npos );
}

TEST( StateManager,
      SnapshotIsStateKind )
{
    const grab::event::StateManager state;

    const auto                      snapshot = state.snapshot( kTimestamp );

    EXPECT_EQ( snapshot.timestamp, kTimestamp );
    EXPECT_EQ( snapshot.sequence, kUnsetSequence );
    EXPECT_EQ( snapshot.kind, kStateSnapshotKind );
    EXPECT_EQ( snapshot.category, kStateCategory );
}

TEST( StateManager,
      IgnoresNonWindowEvents )
{
    grab::event::StateManager state;

    state.observe( make_key_down_event() );

    EXPECT_EQ( state.open_window_count(), kNoOpenWindows );
}

TEST( StateManager,
      PublishSnapshotEmitsOnBus )
{
    grab::EventBus                  bus;
    auto                            subscription = bus.subscribe( grab::EventFilter{
        .kinds      = { kStateSnapshotKind },
        .categories = {},
    } );
    const grab::event::StateManager state;

    state.publish_snapshot( bus, kTimestamp );

    const std::optional<grab::Event> event = subscription.try_pop();
    ASSERT_TRUE( event.has_value() );
    EXPECT_EQ( event->kind, kStateSnapshotKind );

    const auto* payload = state_snapshot_payload( *event );
    ASSERT_NE( payload, nullptr );
    EXPECT_NE( payload->json.find( kOpenKey ), std::string::npos );
}
