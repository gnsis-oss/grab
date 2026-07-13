#pragma once

#include "grab/pid.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace grab
{

    enum class EventCategory : std::uint8_t
    {
        Unspecified   = 0U,
        Input         = 1U,
        Window        = 2U,
        Accessibility = 3U,
        Integration   = 4U,
        Browser       = 5U,
        State         = 6U,
        Count         = 7U,
    };

    enum class EventKind : std::uint16_t
    {
        Unspecified        = 0U,
        KeyDown            = 100U,
        KeyUp              = 101U,
        KeyCombo           = 102U,
        MouseClick         = 103U,
        MouseMove          = 104U,
        IdleStart          = 105U,
        IdleEnd            = 106U,
        WindowFocusChanged = 200U,
        WindowTitleChanged = 201U,
        WindowCreated      = 202U,
        WindowClosed       = 203U,
        A11yButtonClicked  = 300U,
        A11yMenuOpened     = 301U,
        A11yMenuClosed     = 302U,
        A11yFocusChanged   = 303U,
        A11yTextChanged    = 304U,
        A11yStateChanged   = 305U,
        AppTabChanged      = 400U,
        AppContextUpdate   = 401U,
        BrowserTabSwitched = 500U,
        StateSnapshot      = 600U,
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
            grab::Pid   pid;
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
            grab::Pid   pid;
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
            EventKind     kind      = EventKind::Unspecified;
            EventCategory category  = EventCategory::Unspecified;
            Payload       payload;
    };

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
