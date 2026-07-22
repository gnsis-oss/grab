#pragma once

#include "grab/enum_table.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace grab
{

    enum class AvailabilityState : std::uint8_t
    {
        Available,
        Degraded,
        NeedsPermission,
        Unavailable,
        Count,
    };

    namespace capability
    {

        inline constexpr std::string_view screen_display_image = "screen.display.image";
        // reserved (spec §5)
        inline constexpr std::string_view screen_display_video = "screen.display.video";
        inline constexpr std::string_view screen_window_image  = "screen.window.image";
        inline constexpr std::string_view screen_active_window_image =
            "screen.active_window.image";
        inline constexpr std::string_view screen_user_selected_image =
            "screen.user_selected.image";
        inline constexpr std::string_view event_key_global   = "event.key.global";
        inline constexpr std::string_view event_mouse_global = "event.mouse.global";
        inline constexpr std::string_view event_window_focus = "event.window.focus";
        inline constexpr std::string_view event_window_list  = "event.window.list";
        inline constexpr std::string_view window_find        = "window.find";
        inline constexpr std::string_view window_geometry    = "window.geometry";
        // reserved prefix (spec §5)
        inline constexpr std::string_view event_widget_prefix = "event.widget";
        inline constexpr std::string_view mouse_move          = "mouse.move";
        inline constexpr std::string_view mouse_move_absolute = "mouse.move.absolute";
        inline constexpr std::string_view mouse_move_relative = "mouse.move.relative";
        inline constexpr std::string_view mouse_click         = "mouse.click";
        inline constexpr std::string_view mouse_drag          = "mouse.drag";
        inline constexpr std::string_view key_text            = "key.text";
        inline constexpr std::string_view key_chord           = "key.chord";
        inline constexpr std::string_view overlay             = "overlay";

    }    // namespace capability

    enum class Capability : std::uint8_t
    {
        ScreenDisplayImage,
        ScreenDisplayVideo,
        ScreenWindowImage,
        ScreenActiveWindowImage,
        ScreenUserSelectedImage,
        EventKeyGlobal,
        EventMouseGlobal,
        EventWindowFocus,
        EventWindowList,
        WindowFind,
        WindowGeometry,
        MouseMove,
        MouseMoveAbsolute,
        MouseMoveRelative,
        MouseClick,
        MouseDrag,
        KeyText,
        KeyChord,
        Overlay,
        Count,
    };

    namespace detail
    {

        inline constexpr auto availabilityStateNames = EnumTable{
            std::to_array( {
                enum_entry( AvailabilityState::Available, "available" ),
                enum_entry( AvailabilityState::Degraded, "degraded" ),
                enum_entry( AvailabilityState::NeedsPermission, "needs-permission" ),
                enum_entry( AvailabilityState::Unavailable, "unavailable" ),
            } ),
        };
        static_assert( enum_table_has_count( availabilityStateNames,
                                             AvailabilityState::Count ) );

        inline constexpr auto capabilityNames = EnumTable{
            std::to_array( {
                enum_entry( Capability::ScreenDisplayImage,
                            capability::screen_display_image ),
                enum_entry( Capability::ScreenDisplayVideo,
                            capability::screen_display_video ),
                enum_entry( Capability::ScreenWindowImage,
                            capability::screen_window_image ),
                enum_entry( Capability::ScreenActiveWindowImage,
                            capability::screen_active_window_image ),
                enum_entry( Capability::ScreenUserSelectedImage,
                            capability::screen_user_selected_image ),
                enum_entry( Capability::EventKeyGlobal, capability::event_key_global ),
                enum_entry( Capability::EventMouseGlobal,
                            capability::event_mouse_global ),
                enum_entry( Capability::EventWindowFocus,
                            capability::event_window_focus ),
                enum_entry( Capability::EventWindowList, capability::event_window_list ),
                enum_entry( Capability::WindowFind, capability::window_find ),
                enum_entry( Capability::WindowGeometry, capability::window_geometry ),
                enum_entry( Capability::MouseMove, capability::mouse_move ),
                enum_entry( Capability::MouseMoveAbsolute,
                            capability::mouse_move_absolute ),
                enum_entry( Capability::MouseMoveRelative,
                            capability::mouse_move_relative ),
                enum_entry( Capability::MouseClick, capability::mouse_click ),
                enum_entry( Capability::MouseDrag, capability::mouse_drag ),
                enum_entry( Capability::KeyText, capability::key_text ),
                enum_entry( Capability::KeyChord, capability::key_chord ),
                enum_entry( Capability::Overlay, capability::overlay ),
            } ),
        };
        static_assert( enum_table_has_count( capabilityNames,
                                             Capability::Count ) );

    }    // namespace detail

    [[nodiscard]]
    constexpr std::string_view
    state_name( AvailabilityState state ) noexcept
    {
        return detail::availabilityStateNames.text_of( state, "unavailable" );
    }

    [[nodiscard]]
    constexpr std::string_view
    capability_name( Capability capability_id ) noexcept
    {
        return detail::capabilityNames.text_of( capability_id, "" );
    }

    [[nodiscard]]
    constexpr std::optional<Capability>
    capability_from_string( std::string_view capability_id ) noexcept
    {
        return detail::capabilityNames.value_of( capability_id );
    }

    [[nodiscard]]
    constexpr const auto&
    capability_entries() noexcept
    {
        return detail::capabilityNames.entries;
    }

    struct Availability
    {
            AvailabilityState state = AvailabilityState::Unavailable;
            std::string       reason;
            int               quality = 0;
    };

    struct CapabilityDescriptor
    {
            Capability        id = Capability::ScreenDisplayImage;
            std::string       provider;
            int               contract_version = 1;
            AvailabilityState state            = AvailabilityState::Unavailable;
            std::string       degradation_reason;
            std::string       required_permission;
    };

}    // namespace grab
