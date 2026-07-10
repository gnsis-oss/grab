#pragma once

#include "grab/enum_table.hpp"
#include "grab/event.hpp"

#include <array>
#include <optional>
#include <string_view>

namespace grab
{

    struct EventDescriptor
    {
            EventKind        kind;
            EventCategory    category;
            std::string_view wire_name;
    };

    namespace detail
    {

        inline constexpr std::array<EventDescriptor, 22> eventDescriptors{
            {
             { EventKind::Unspecified, EventCategory::Unspecified, "unspecified" },
             { EventKind::KeyDown, EventCategory::Input, "input.key_down" },
             { EventKind::KeyUp, EventCategory::Input, "input.key_up" },
             { EventKind::KeyCombo, EventCategory::Input, "input.key_combo" },
             { EventKind::MouseClick, EventCategory::Input, "input.mouse_click" },
             { EventKind::MouseMove, EventCategory::Input, "input.mouse_move" },
             { EventKind::IdleStart, EventCategory::Input, "input.idle_start" },
             { EventKind::IdleEnd, EventCategory::Input, "input.idle_end" },
             { EventKind::WindowFocusChanged,
                  EventCategory::Window,
                  "window.focus_changed" },
             { EventKind::WindowTitleChanged,
                  EventCategory::Window,
                  "window.title_changed" },
             { EventKind::WindowCreated, EventCategory::Window, "window.created" },
             { EventKind::WindowClosed, EventCategory::Window, "window.closed" },
             { EventKind::A11yButtonClicked,
                  EventCategory::Accessibility,
                  "a11y.button_clicked" },
             { EventKind::A11yMenuOpened,
                  EventCategory::Accessibility,
                  "a11y.menu_opened" },
             { EventKind::A11yMenuClosed,
                  EventCategory::Accessibility,
                  "a11y.menu_closed" },
             { EventKind::A11yFocusChanged,
                  EventCategory::Accessibility,
                  "a11y.focus_changed" },
             { EventKind::A11yTextChanged,
                  EventCategory::Accessibility,
                  "a11y.text_changed" },
             { EventKind::A11yStateChanged,
                  EventCategory::Accessibility,
                  "a11y.state_changed" },
             { EventKind::AppTabChanged,
                  EventCategory::Integration,
                  "app.tab_changed" },
             { EventKind::AppContextUpdate,
                  EventCategory::Integration,
                  "app.context_update" },
             { EventKind::BrowserTabSwitched,
                  EventCategory::Browser,
                  "browser.tab_switched" },
             { EventKind::StateSnapshot, EventCategory::State, "state.snapshot" },
             }
        };
        static_assert( eventDescriptors.size() == 22U );

        [[nodiscard]]
        constexpr EventCategory
        oracle_category( EventKind kind ) noexcept
        {
            switch( kind )
            {
                case EventKind::KeyDown :
                case EventKind::KeyUp :
                case EventKind::KeyCombo :
                case EventKind::MouseClick :
                case EventKind::MouseMove :
                case EventKind::IdleStart :
                case EventKind::IdleEnd :
                    return EventCategory::Input;
                case EventKind::WindowFocusChanged :
                case EventKind::WindowTitleChanged :
                case EventKind::WindowCreated :
                case EventKind::WindowClosed :
                    return EventCategory::Window;
                case EventKind::A11yButtonClicked :
                case EventKind::A11yMenuOpened :
                case EventKind::A11yMenuClosed :
                case EventKind::A11yFocusChanged :
                case EventKind::A11yTextChanged :
                case EventKind::A11yStateChanged :
                    return EventCategory::Accessibility;
                case EventKind::AppTabChanged :
                case EventKind::AppContextUpdate :
                    return EventCategory::Integration;
                case EventKind::BrowserTabSwitched :
                    return EventCategory::Browser;
                case EventKind::StateSnapshot :
                    return EventCategory::State;
                case EventKind::Unspecified :
                    return EventCategory::Unspecified;
            }

            return EventCategory::Unspecified;
        }

        static_assert(
            []
            {
                for( const auto& descriptor : eventDescriptors )
                {
                    if( descriptor.category != oracle_category( descriptor.kind ) )
                    {
                        return false;
                    }
                }
                return true;
            }()
        );

        inline constexpr auto categoryNames = EnumTable{
            std::to_array( {
                enum_entry( EventCategory::Unspecified, "unspecified" ),
                enum_entry( EventCategory::Input, "input" ),
                enum_entry( EventCategory::Window, "window" ),
                enum_entry( EventCategory::Accessibility, "accessibility" ),
                enum_entry( EventCategory::Integration, "integration" ),
                enum_entry( EventCategory::Browser, "browser" ),
                enum_entry( EventCategory::State, "state" ),
            } ),
        };
        static_assert( enum_table_has_count( categoryNames,
                                             7U ) );

    }    // namespace detail

    inline constexpr std::string_view unspecifiedWireName = "unspecified";

    [[nodiscard]]
    constexpr EventCategory
    category_of( EventKind kind ) noexcept
    {
        for( const auto& descriptor : detail::eventDescriptors )
        {
            if( descriptor.kind == kind )
            {
                return descriptor.category;
            }
        }
        return EventCategory::Unspecified;
    }

    [[nodiscard]]
    constexpr std::string_view
    wire_name( EventKind kind ) noexcept
    {
        for( const auto& descriptor : detail::eventDescriptors )
        {
            if( descriptor.kind == kind )
            {
                return descriptor.wire_name;
            }
        }
        return unspecifiedWireName;
    }

    [[nodiscard]]
    constexpr std::optional<EventKind>
    wire_kind( std::string_view name ) noexcept
    {
        for( const auto& descriptor : detail::eventDescriptors )
        {
            if( descriptor.wire_name == name )
            {
                return descriptor.kind;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]]
    constexpr std::string_view
    category_name( EventCategory category ) noexcept
    {
        return detail::categoryNames.text_of( category, unspecifiedWireName );
    }

}    // namespace grab
