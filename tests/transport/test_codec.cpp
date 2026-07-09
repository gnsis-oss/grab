#include "eventgrab/v1/events.pb.h"
#include "grab/event.hpp"
#include "grab/result.hpp"
#include "transport/codec.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
// clang-format on

namespace
{

    constexpr double        kTimestamp           = 1729.125;
    constexpr double        kExpectedTimestamp   = kTimestamp;
    constexpr double        kMouseDelta          = -4.5;
    constexpr double        kIdleSeconds         = 30.25;
    constexpr double        kWindowDuration      = 7.75;
    constexpr std::uint64_t kSequence            = 42U;
    constexpr std::uint64_t kDecodedSequence     = 0U;
    constexpr std::uint32_t kKeyCode             = 30U;
    constexpr std::uint32_t kMouseButton         = 1U;
    constexpr int           kUnknownKindNumber   = 9'999;
    constexpr int           kOversizedEntryPad   = 1;
    constexpr int           kSingleErasedEntry   = 1;
    constexpr auto          kProtocolErrorCode   = grab::ErrorCode::protocol_error;
    constexpr auto          kInputCategory       = grab::EventCategory::input;
    constexpr auto          kWindowCategory      = grab::EventCategory::window;
    constexpr auto          kA11yCategory        = grab::EventCategory::accessibility;
    constexpr auto          kIntegrationCategory = grab::EventCategory::integration;
    constexpr auto          kBrowserCategory     = grab::EventCategory::browser;
    constexpr auto          kStateCategory       = grab::EventCategory::state;

    [[nodiscard]]
    grab::InputKey
    input_key()
    {
        return grab::InputKey{ .code = kKeyCode, .name = "A" };
    }

    [[nodiscard]]
    grab::KeyCombo
    key_combo()
    {
        return grab::KeyCombo{ .text = "Ctrl+A" };
    }

    [[nodiscard]]
    grab::MouseClick
    mouse_click()
    {
        return grab::MouseClick{ .button = kMouseButton, .name = "left" };
    }

    [[nodiscard]]
    grab::MouseMove
    mouse_move()
    {
        return grab::MouseMove{ .axis = "0", .delta = kMouseDelta };
    }

    [[nodiscard]]
    grab::Idle
    idle()
    {
        return grab::Idle{ .idle_s = kIdleSeconds };
    }

    [[nodiscard]]
    grab::WindowChange
    window_change()
    {
        return grab::WindowChange{
            .app        = "Firefox",
            .pid        = "1234",
            .title      = "New title",
            .prev_title = "Old title",
            .duration_s = kWindowDuration,
        };
    }

    [[nodiscard]]
    grab::A11yEvent
    a11y_event()
    {
        return grab::A11yEvent{
            .app    = "Editor",
            .role   = "push button",
            .name   = "Save",
            .detail = "pressed",
        };
    }

    [[nodiscard]]
    grab::IntegrationEvent
    integration_event()
    {
        return grab::IntegrationEvent{
            .app    = "Browser",
            .title  = "Inbox",
            .detail = "context",
            .json   = R"({"tab":1})",
        };
    }

    [[nodiscard]]
    grab::BrowserTab
    browser_tab()
    {
        return grab::BrowserTab{
            .app            = "Firefox",
            .pid            = "1234",
            .tab_title      = "Docs",
            .prev_tab_title = "Search",
        };
    }

    [[nodiscard]]
    grab::StateSnapshot
    state_snapshot()
    {
        return grab::StateSnapshot{ .json = R"({"focused_app":"Firefox"})" };
    }

    [[nodiscard]]
    grab::Event
    make_event( grab::EventKind     kind,
                grab::EventCategory category,
                grab::Payload       payload )
    {
        return grab::Event{
            .timestamp = kTimestamp,
            .sequence  = kSequence,
            .kind      = kind,
            .category  = category,
            .payload   = std::move( payload ),
        };
    }

    [[nodiscard]]
    std::vector<grab::Event>
    all_payload_events()
    {
        return {
            make_event( grab::EventKind::key_down, kInputCategory, input_key() ),
            make_event( grab::EventKind::key_up, kInputCategory, input_key() ),
            make_event( grab::EventKind::key_combo, kInputCategory, key_combo() ),
            make_event( grab::EventKind::mouse_click, kInputCategory, mouse_click() ),
            make_event( grab::EventKind::mouse_move, kInputCategory, mouse_move() ),
            make_event( grab::EventKind::idle_start, kInputCategory, idle() ),
            make_event( grab::EventKind::idle_end, kInputCategory, idle() ),
            make_event( grab::EventKind::window_focus_changed,
                        kWindowCategory,
                        window_change() ),
            make_event( grab::EventKind::window_title_changed,
                        kWindowCategory,
                        window_change() ),
            make_event( grab::EventKind::window_created,
                        kWindowCategory,
                        window_change() ),
            make_event( grab::EventKind::window_closed,
                        kWindowCategory,
                        window_change() ),
            make_event( grab::EventKind::a11y_button_clicked,
                        kA11yCategory,
                        a11y_event() ),
            make_event( grab::EventKind::a11y_menu_opened, kA11yCategory, a11y_event() ),
            make_event( grab::EventKind::a11y_menu_closed, kA11yCategory, a11y_event() ),
            make_event( grab::EventKind::a11y_focus_changed,
                        kA11yCategory,
                        a11y_event() ),
            make_event( grab::EventKind::a11y_text_changed,
                        kA11yCategory,
                        a11y_event() ),
            make_event( grab::EventKind::a11y_state_changed,
                        kA11yCategory,
                        a11y_event() ),
            make_event( grab::EventKind::app_tab_changed,
                        kIntegrationCategory,
                        integration_event() ),
            make_event( grab::EventKind::app_context_update,
                        kIntegrationCategory,
                        integration_event() ),
            make_event( grab::EventKind::browser_tab_switched,
                        kBrowserCategory,
                        browser_tab() ),
            make_event( grab::EventKind::state_snapshot,
                        kStateCategory,
                        state_snapshot() ),
        };
    }

    void
    expect_payload_value_eq( const grab::InputKey& expected,
                             const grab::InputKey& actual )
    {
        EXPECT_EQ( actual.code, expected.code );
        EXPECT_EQ( actual.name, expected.name );
    }

    void
    expect_payload_value_eq( const grab::KeyCombo& expected,
                             const grab::KeyCombo& actual )
    {
        EXPECT_EQ( actual.text, expected.text );
    }

    void
    expect_payload_value_eq( const grab::MouseClick& expected,
                             const grab::MouseClick& actual )
    {
        EXPECT_EQ( actual.button, expected.button );
        EXPECT_EQ( actual.name, expected.name );
    }

    void
    expect_payload_value_eq( const grab::MouseMove& expected,
                             const grab::MouseMove& actual )
    {
        EXPECT_EQ( actual.axis, expected.axis );
        EXPECT_DOUBLE_EQ( actual.delta, expected.delta );
    }

    void
    expect_payload_value_eq( const grab::Idle& expected,
                             const grab::Idle& actual )
    {
        EXPECT_DOUBLE_EQ( actual.idle_s, expected.idle_s );
    }

    void
    expect_payload_value_eq( const grab::WindowChange& expected,
                             const grab::WindowChange& actual )
    {
        EXPECT_EQ( actual.app, expected.app );
        EXPECT_EQ( actual.pid, expected.pid );
        EXPECT_EQ( actual.title, expected.title );
        EXPECT_EQ( actual.prev_title, expected.prev_title );
        EXPECT_DOUBLE_EQ( actual.duration_s, expected.duration_s );
    }

    void
    expect_payload_value_eq( const grab::A11yEvent& expected,
                             const grab::A11yEvent& actual )
    {
        EXPECT_EQ( actual.app, expected.app );
        EXPECT_EQ( actual.role, expected.role );
        EXPECT_EQ( actual.name, expected.name );
        EXPECT_EQ( actual.detail, expected.detail );
    }

    void
    expect_payload_value_eq( const grab::IntegrationEvent& expected,
                             const grab::IntegrationEvent& actual )
    {
        EXPECT_EQ( actual.app, expected.app );
        EXPECT_EQ( actual.title, expected.title );
        EXPECT_EQ( actual.detail, expected.detail );
        EXPECT_EQ( actual.json, expected.json );
    }

    void
    expect_payload_value_eq( const grab::BrowserTab& expected,
                             const grab::BrowserTab& actual )
    {
        EXPECT_EQ( actual.app, expected.app );
        EXPECT_EQ( actual.pid, expected.pid );
        EXPECT_EQ( actual.tab_title, expected.tab_title );
        EXPECT_EQ( actual.prev_tab_title, expected.prev_tab_title );
    }

    void
    expect_payload_value_eq( const grab::StateSnapshot& expected,
                             const grab::StateSnapshot& actual )
    {
        EXPECT_EQ( actual.json, expected.json );
    }

    void
    expect_payload_eq( const grab::Payload& expected,
                       const grab::Payload& actual )
    {
        ASSERT_EQ( expected.index(), actual.index() );

        std::visit(
            []( const auto& lhs, const auto& rhs )
            {
                using Left  = std::decay_t<decltype( lhs )>;
                using Right = std::decay_t<decltype( rhs )>;
                if constexpr( std::is_same_v<Left, Right> )
                {
                    expect_payload_value_eq( lhs, rhs );
                }
            },
            expected,
            actual
        );
    }

    void
    expect_event_eq_after_wire( const grab::Event& expected,
                                const grab::Event& actual )
    {
        EXPECT_EQ( actual.kind, expected.kind );
        EXPECT_EQ( actual.category, expected.category );
        EXPECT_DOUBLE_EQ( actual.timestamp, expected.timestamp );
        EXPECT_EQ( actual.sequence, kDecodedSequence );
        expect_payload_eq( expected.payload, actual.payload );
    }

    [[nodiscard]]
    eventgrab::v1::Event
    valid_key_down_wire()
    {
        const auto event =
            make_event( grab::EventKind::key_down, kInputCategory, input_key() );
        auto wire = grab::transport::to_wire( event );
        EXPECT_TRUE( wire.has_value() );
        return *wire;
    }

}    // namespace

TEST( Codec,
      RoundTripsEveryKind )
{
    for( const auto& event : all_payload_events() )
    {
        auto wire = grab::transport::to_wire( event );
        ASSERT_TRUE( wire.has_value() );

        auto decoded = grab::transport::from_wire( *wire );
        ASSERT_TRUE( decoded.has_value() );
        expect_event_eq_after_wire( event, *decoded );
    }
}

TEST( Codec,
      UnknownKindRejected )
{
    eventgrab::v1::Event unspecified_wire = valid_key_down_wire();
    unspecified_wire.set_kind( eventgrab::v1::EVENT_KIND_UNSPECIFIED );

    const auto unspecified_decoded = grab::transport::from_wire( unspecified_wire );

    ASSERT_FALSE( unspecified_decoded.has_value() );
    EXPECT_EQ( unspecified_decoded.error().code, kProtocolErrorCode );

    eventgrab::v1::Event unknown_wire = valid_key_down_wire();
    unknown_wire.set_kind( static_cast<eventgrab::v1::EventKind>( kUnknownKindNumber ) );

    const auto unknown_decoded = grab::transport::from_wire( unknown_wire );

    ASSERT_FALSE( unknown_decoded.has_value() );
    EXPECT_EQ( unknown_decoded.error().code, kProtocolErrorCode );
}

TEST( Codec,
      MissingRequiredFieldRejected )
{
    eventgrab::v1::Event wire = valid_key_down_wire();
    EXPECT_EQ( wire.mutable_data()->erase( "key_code" ), kSingleErasedEntry );

    const auto decoded = grab::transport::from_wire( wire );

    ASSERT_FALSE( decoded.has_value() );
    EXPECT_EQ( decoded.error().code, kProtocolErrorCode );
}

TEST( Codec,
      OversizedDataRejected )
{
    eventgrab::v1::Event wire = valid_key_down_wire();
    for( int index = 0; index < grab::transport::kMaxDataEntries + kOversizedEntryPad;
         ++index )
    {
        const auto [iter, inserted] =
            wire.mutable_data()->try_emplace( "extra_" + std::to_string( index ),
                                              "value" );
        EXPECT_TRUE( inserted );
        EXPECT_EQ( iter->second, "value" );
    }

    const auto decoded = grab::transport::from_wire( wire );

    ASSERT_FALSE( decoded.has_value() );
    EXPECT_EQ( decoded.error().code, kProtocolErrorCode );
}

TEST( Codec,
      TimestampPreserved )
{
    const auto event =
        make_event( grab::EventKind::mouse_move, kInputCategory, mouse_move() );

    auto wire = grab::transport::to_wire( event );
    ASSERT_TRUE( wire.has_value() );
    EXPECT_DOUBLE_EQ( wire->timestamp(), kExpectedTimestamp );

    auto decoded = grab::transport::from_wire( *wire );
    ASSERT_TRUE( decoded.has_value() );
    EXPECT_DOUBLE_EQ( decoded->timestamp, kExpectedTimestamp );
    EXPECT_EQ( decoded->sequence, kDecodedSequence );
}
