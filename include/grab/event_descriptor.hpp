#pragma once

#include "grab/enum_table.hpp"
#include "grab/event.hpp"

#include <array>
#include <optional>
#include <string_view>

namespace grab
{

    enum class ReplayPolicy : std::uint8_t
    {
        None       = 0U,
        CurrentSet = 1U,
        Count      = 2U,
    };

    enum class CoalescingClass : std::uint8_t
    {
        Coalesce  = 0U,
        NeverDrop = 1U,
        Count     = 2U,
    };

    struct EventDescriptor
    {
            EventKind        kind;
            EventCategory    category;
            std::string_view wire_name;
            ReplayPolicy     replay_policy{ ReplayPolicy::None };
            CoalescingClass  coalescing_class{ CoalescingClass::Coalesce };
    };

    namespace detail
    {

        [[nodiscard]]
        constexpr EventDescriptor
        event_descriptor( EventKind        kind,
                          EventCategory    category,
                          std::string_view wire_name,
                          ReplayPolicy     replay_policy = ReplayPolicy::None,
                          CoalescingClass  coalescing_class =
                              CoalescingClass::Coalesce ) noexcept
        {
            return EventDescriptor{
                .kind             = kind,
                .category         = category,
                .wire_name        = wire_name,
                .replay_policy    = replay_policy,
                .coalescing_class = coalescing_class,
            };
        }

        inline constexpr auto eventDescriptors  = std::to_array<EventDescriptor>( {
            event_descriptor( EventKind::Unspecified,
                              EventCategory::Unspecified,
                              "unspecified" ),
            event_descriptor( EventKind::KeyDown,
                              EventCategory::Input,
                              "input.key_down",
                              ReplayPolicy::None,
                              CoalescingClass::NeverDrop ),
            event_descriptor( EventKind::KeyUp,
                              EventCategory::Input,
                              "input.key_up",
                              ReplayPolicy::None,
                              CoalescingClass::NeverDrop ),
            event_descriptor( EventKind::KeyCombo,
                              EventCategory::Input,
                              "input.key_combo",
                              ReplayPolicy::None,
                              CoalescingClass::NeverDrop ),
            event_descriptor( EventKind::MouseClick,
                              EventCategory::Input,
                              "input.mouse_click",
                              ReplayPolicy::None,
                              CoalescingClass::NeverDrop ),
            event_descriptor( EventKind::MouseMove,
                              EventCategory::Input,
                              "input.mouse_move",
                              ReplayPolicy::None,
                              CoalescingClass::Coalesce ),
            event_descriptor( EventKind::IdleStart,
                              EventCategory::Input,
                              "input.idle_start",
                              ReplayPolicy::None,
                              CoalescingClass::NeverDrop ),
            event_descriptor( EventKind::IdleEnd,
                              EventCategory::Input,
                              "input.idle_end",
                              ReplayPolicy::None,
                              CoalescingClass::NeverDrop ),
            event_descriptor( EventKind::WindowFocusChanged,
                              EventCategory::Window,
                              "window.focus_changed" ),
            event_descriptor( EventKind::WindowTitleChanged,
                              EventCategory::Window,
                              "window.title_changed" ),
            event_descriptor( EventKind::WindowCreated,
                              EventCategory::Window,
                              "window.created",
                              ReplayPolicy::None,
                              CoalescingClass::NeverDrop ),
            event_descriptor( EventKind::WindowClosed,
                              EventCategory::Window,
                              "window.closed",
                              ReplayPolicy::None,
                              CoalescingClass::NeverDrop ),
            event_descriptor( EventKind::A11yButtonClicked,
                              EventCategory::Accessibility,
                              "a11y.button_clicked",
                              ReplayPolicy::None,
                              CoalescingClass::NeverDrop ),
            event_descriptor( EventKind::A11yMenuOpened,
                              EventCategory::Accessibility,
                              "a11y.menu_opened",
                              ReplayPolicy::None,
                              CoalescingClass::NeverDrop ),
            event_descriptor( EventKind::A11yMenuClosed,
                              EventCategory::Accessibility,
                              "a11y.menu_closed",
                              ReplayPolicy::None,
                              CoalescingClass::NeverDrop ),
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
                              "state.snapshot",
                              ReplayPolicy::CurrentSet ),
        } );

        inline constexpr auto replayPolicyNames = EnumTable{
            std::to_array( {
                enum_entry( ReplayPolicy::None, "none" ),
                enum_entry( ReplayPolicy::CurrentSet, "current_set" ),
            } ),
        };
        static_assert( enum_table_has_count( replayPolicyNames,
                                             ReplayPolicy::Count ) );

        inline constexpr auto coalescingClassNames = EnumTable{
            std::to_array( {
                enum_entry( CoalescingClass::Coalesce, "coalesce" ),
                enum_entry( CoalescingClass::NeverDrop, "never_drop" ),
            } ),
        };
        static_assert( enum_table_has_count( coalescingClassNames,
                                             CoalescingClass::Count ) );

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
    constexpr ReplayPolicy
    replay_policy_of( EventKind kind ) noexcept
    {
        for( const auto& descriptor : detail::eventDescriptors )
        {
            if( descriptor.kind == kind )
            {
                return descriptor.replay_policy;
            }
        }
        return ReplayPolicy::None;
    }

    [[nodiscard]]
    constexpr CoalescingClass
    coalescing_class_of( EventKind kind ) noexcept
    {
        for( const auto& descriptor : detail::eventDescriptors )
        {
            if( descriptor.kind == kind )
            {
                return descriptor.coalescing_class;
            }
        }
        return CoalescingClass::Coalesce;
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
