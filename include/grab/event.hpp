#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace grab
{

    enum class EventCategory : std::uint8_t
    {
        unspecified   = 0U,
        input         = 1U,
        window        = 2U,
        accessibility = 3U,
        integration   = 4U,
        browser       = 5U,
        state         = 6U,
    };

    enum class EventKind : std::uint16_t
    {
        unspecified          = 0U,
        key_down             = 100U,
        key_up               = 101U,
        key_combo            = 102U,
        mouse_click          = 103U,
        mouse_move           = 104U,
        idle_start           = 105U,
        idle_end             = 106U,
        window_focus_changed = 200U,
        window_title_changed = 201U,
        window_created       = 202U,
        window_closed        = 203U,
        a11y_button_clicked  = 300U,
        a11y_menu_opened     = 301U,
        a11y_menu_closed     = 302U,
        a11y_focus_changed   = 303U,
        a11y_text_changed    = 304U,
        a11y_state_changed   = 305U,
        app_tab_changed      = 400U,
        app_context_update   = 401U,
        browser_tab_switched = 500U,
        state_snapshot       = 600U,
    };

    struct InputKey
    {
            std::uint32_t code = 0U;
            std::string   name;
    };

    struct KeyCombo
    {
            std::string text;
    };

    struct MouseClick
    {
            std::uint32_t button = 0U;
            std::string   name;
    };

    struct MouseMove
    {
            std::string axis;
            double      delta = 0.0;
    };

    struct Idle
    {
            double idle_s = 0.0;
    };

    struct WindowChange
    {
            std::string app;
            std::string pid;
            std::string title;
            std::string prev_title;
            double      duration_s = 0.0;
    };

    struct A11yEvent
    {
            std::string app;
            std::string role;
            std::string name;
            std::string detail;
    };

    struct IntegrationEvent
    {
            std::string app;
            std::string title;
            std::string detail;
            std::string json;
    };

    struct BrowserTab
    {
            std::string app;
            std::string pid;
            std::string tab_title;
            std::string prev_tab_title;
    };

    struct StateSnapshot
    {
            std::string json;
    };

    using Payload = std::variant<InputKey,
                                 KeyCombo,
                                 MouseClick,
                                 MouseMove,
                                 Idle,
                                 WindowChange,
                                 A11yEvent,
                                 IntegrationEvent,
                                 BrowserTab,
                                 StateSnapshot>;

    struct Event
    {
            double        timestamp = 0.0;
            std::uint64_t sequence  = 0U;
            EventKind     kind      = EventKind::unspecified;
            EventCategory category  = EventCategory::unspecified;
            Payload       payload;
    };

    [[nodiscard]]
    constexpr EventCategory
    category_of( EventKind kind ) noexcept
    {
        switch( kind )
        {
            case EventKind::key_down :
            case EventKind::key_up :
            case EventKind::key_combo :
            case EventKind::mouse_click :
            case EventKind::mouse_move :
            case EventKind::idle_start :
            case EventKind::idle_end :
                return EventCategory::input;
            case EventKind::window_focus_changed :
            case EventKind::window_title_changed :
            case EventKind::window_created :
            case EventKind::window_closed :
                return EventCategory::window;
            case EventKind::a11y_button_clicked :
            case EventKind::a11y_menu_opened :
            case EventKind::a11y_menu_closed :
            case EventKind::a11y_focus_changed :
            case EventKind::a11y_text_changed :
            case EventKind::a11y_state_changed :
                return EventCategory::accessibility;
            case EventKind::app_tab_changed :
            case EventKind::app_context_update :
                return EventCategory::integration;
            case EventKind::browser_tab_switched :
                return EventCategory::browser;
            case EventKind::state_snapshot :
                return EventCategory::state;
            case EventKind::unspecified :
                return EventCategory::unspecified;
        }

        return EventCategory::unspecified;
    }

    struct EventFilter
    {
            std::vector<EventKind>     kinds;
            std::vector<EventCategory> categories;

            [[nodiscard]]
            bool
            matches( const Event& event ) const noexcept
            {
                const bool kind_matches = kinds.empty() ||
                                          std::ranges::find( kinds, event.kind ) !=
                                          kinds.end();
                const bool category_matches =
                    categories.empty() ||
                    std::ranges::find( categories, event.category ) != categories.end();
                return kind_matches && category_matches;
            }
    };

}    // namespace grab
