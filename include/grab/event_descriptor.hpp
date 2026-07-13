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

        [[nodiscard]]
        constexpr EventDescriptor
        event_descriptor( EventKind        kind,
                          EventCategory    category,
                          std::string_view wire_name ) noexcept
        {
            return EventDescriptor{
                .kind      = kind,
                .category  = category,
                .wire_name = wire_name,
            };
        }

        inline constexpr auto eventDescriptors = std::to_array<EventDescriptor>( {
            event_descriptor( EventKind::Unspecified,
                              EventCategory::Unspecified,
                              "unspecified" ),
            event_descriptor( EventKind::KeyDown,
                              EventCategory::Input,
                              "input.key_down" ),
            event_descriptor( EventKind::KeyUp, EventCategory::Input, "input.key_up" ),
            event_descriptor( EventKind::KeyCombo,
                              EventCategory::Input,
                              "input.key_combo" ),
            event_descriptor( EventKind::MouseClick,
                              EventCategory::Input,
                              "input.mouse_click" ),
            event_descriptor( EventKind::MouseMove,
                              EventCategory::Input,
                              "input.mouse_move" ),
            event_descriptor( EventKind::IdleStart,
                              EventCategory::Input,
                              "input.idle_start" ),
            event_descriptor( EventKind::IdleEnd,
                              EventCategory::Input,
                              "input.idle_end" ),
            event_descriptor( EventKind::WindowFocusChanged,
                              EventCategory::Window,
                              "window.focus_changed" ),
            event_descriptor( EventKind::WindowTitleChanged,
                              EventCategory::Window,
                              "window.title_changed" ),
            event_descriptor( EventKind::WindowCreated,
                              EventCategory::Window,
                              "window.created" ),
            event_descriptor( EventKind::WindowClosed,
                              EventCategory::Window,
                              "window.closed" ),
            event_descriptor( EventKind::A11yButtonClicked,
                              EventCategory::Accessibility,
                              "a11y.button_clicked" ),
            event_descriptor( EventKind::A11yMenuOpened,
                              EventCategory::Accessibility,
                              "a11y.menu_opened" ),
            event_descriptor( EventKind::A11yMenuClosed,
                              EventCategory::Accessibility,
                              "a11y.menu_closed" ),
            event_descriptor( EventKind::A11yFocusChanged,
                              EventCategory::Accessibility,
                              "a11y.focus_changed" ),
            event_descriptor( EventKind::A11yTextChanged,
                              EventCategory::Accessibility,
                              "a11y.text_changed" ),
            event_descriptor( EventKind::A11yStateChanged,
                              EventCategory::Accessibility,
                              "a11y.state_changed" ),
            event_descriptor( EventKind::AppTabChanged,
                              EventCategory::Integration,
                              "app.tab_changed" ),
            event_descriptor( EventKind::AppContextUpdate,
                              EventCategory::Integration,
                              "app.context_update" ),
            event_descriptor( EventKind::BrowserTabSwitched,
                              EventCategory::Browser,
                              "browser.tab_switched" ),
            event_descriptor( EventKind::StateSnapshot,
                              EventCategory::State,
                              "state.snapshot" ),
        } );

        inline constexpr auto categoryNames    = EnumTable{
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
                                             EventCategory::Count ) );

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
