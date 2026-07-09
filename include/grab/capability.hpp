#ifndef GRAB_CAPABILITY_HPP
#define GRAB_CAPABILITY_HPP

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
        available,
        degraded,
        needs_permission,
        unavailable,
        count,
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

    }    // namespace capability

    enum class Capability : std::uint8_t
    {
        screen_display_image,
        screen_display_video,
        screen_window_image,
        screen_active_window_image,
        screen_user_selected_image,
        event_key_global,
        event_mouse_global,
        event_window_focus,
        event_window_list,
        window_find,
        window_geometry,
        mouse_move,
        mouse_move_absolute,
        mouse_move_relative,
        mouse_click,
        mouse_drag,
        key_text,
        key_chord,
        count,
    };

    namespace detail
    {

        inline constexpr auto kAvailabilityStateNames = EnumTable{
            std::to_array( {
                enum_entry( AvailabilityState::available, "available" ),
                enum_entry( AvailabilityState::degraded, "degraded" ),
                enum_entry( AvailabilityState::needs_permission, "needs-permission" ),
                enum_entry( AvailabilityState::unavailable, "unavailable" ),
            } ),
        };
        static_assert( enum_table_has_count( kAvailabilityStateNames,
                                             AvailabilityState::count ) );

        inline constexpr auto kCapabilityNames = EnumTable{
            std::to_array( {
                enum_entry( Capability::screen_display_image,
                            capability::screen_display_image ),
                enum_entry( Capability::screen_display_video,
                            capability::screen_display_video ),
                enum_entry( Capability::screen_window_image,
                            capability::screen_window_image ),
                enum_entry( Capability::screen_active_window_image,
                            capability::screen_active_window_image ),
                enum_entry( Capability::screen_user_selected_image,
                            capability::screen_user_selected_image ),
                enum_entry( Capability::event_key_global, capability::event_key_global ),
                enum_entry( Capability::event_mouse_global,
                            capability::event_mouse_global ),
                enum_entry( Capability::event_window_focus,
                            capability::event_window_focus ),
                enum_entry( Capability::event_window_list,
                            capability::event_window_list ),
                enum_entry( Capability::window_find, capability::window_find ),
                enum_entry( Capability::window_geometry, capability::window_geometry ),
                enum_entry( Capability::mouse_move, capability::mouse_move ),
                enum_entry( Capability::mouse_move_absolute,
                            capability::mouse_move_absolute ),
                enum_entry( Capability::mouse_move_relative,
                            capability::mouse_move_relative ),
                enum_entry( Capability::mouse_click, capability::mouse_click ),
                enum_entry( Capability::mouse_drag, capability::mouse_drag ),
                enum_entry( Capability::key_text, capability::key_text ),
                enum_entry( Capability::key_chord, capability::key_chord ),
            } ),
        };
        static_assert( enum_table_has_count( kCapabilityNames,
                                             Capability::count ) );

    }    // namespace detail

    [[nodiscard]]
    constexpr std::string_view
    state_name( AvailabilityState state ) noexcept
    {
        return detail::kAvailabilityStateNames.text_of( state, "unavailable" );
    }

    [[nodiscard]]
    constexpr std::string_view
    capability_name( Capability capability_id ) noexcept
    {
        return detail::kCapabilityNames.text_of( capability_id, "" );
    }

    [[nodiscard]]
    constexpr std::optional<Capability>
    capability_from_string( std::string_view capability_id ) noexcept
    {
        return detail::kCapabilityNames.value_of( capability_id );
    }

    [[nodiscard]]
    constexpr const auto&
    capability_entries() noexcept
    {
        return detail::kCapabilityNames.entries;
    }

    struct Availability
    {
            AvailabilityState state = AvailabilityState::unavailable;
            std::string       reason;
            int               quality = 0;
    };

    struct CapabilityDescriptor
    {
            Capability        id = Capability::screen_display_image;
            std::string       provider;
            int               contract_version = 1;
            AvailabilityState state            = AvailabilityState::unavailable;
            std::string       degradation_reason;
            std::string       required_permission;
    };

}    // namespace grab

#endif    // GRAB_CAPABILITY_HPP
