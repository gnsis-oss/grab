#pragma once

#include "grab/enum_table.hpp"
#include "grab/event.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace grab
{

    namespace detail
    {

        // Single source of truth for the on-the-wire type string of every
        // grab::EventKind. The JSONL storage sink emits these strings and the
        // browser native-messaging bridge parses them; both derive from this one
        // table, so the serialize and parse vocabularies can never drift apart.
        //
        // Adding an EventKind is caught at compile time by the exhaustive
        // switch in grab::category_of (event.hpp, built under -Wswitch -Werror);
        // give the new kind a row here at the same time. kEventKindWireNameCount
        // then guards against an accidentally dropped or duplicated row.
        inline constexpr auto kEventKindWireNames = EnumTable{
            std::to_array( {
                enum_entry( EventKind::unspecified, "unspecified" ),
                enum_entry( EventKind::key_down, "input.key_down" ),
                enum_entry( EventKind::key_up, "input.key_up" ),
                enum_entry( EventKind::key_combo, "input.key_combo" ),
                enum_entry( EventKind::mouse_click, "input.mouse_click" ),
                enum_entry( EventKind::mouse_move, "input.mouse_move" ),
                enum_entry( EventKind::idle_start, "input.idle_start" ),
                enum_entry( EventKind::idle_end, "input.idle_end" ),
                enum_entry( EventKind::window_focus_changed,
                            "window.focus_changed" ),
                enum_entry( EventKind::window_title_changed,
                            "window.title_changed" ),
                enum_entry( EventKind::window_created, "window.created" ),
                enum_entry( EventKind::window_closed, "window.closed" ),
                enum_entry( EventKind::a11y_button_clicked,
                            "a11y.button_clicked" ),
                enum_entry( EventKind::a11y_menu_opened, "a11y.menu_opened" ),
                enum_entry( EventKind::a11y_menu_closed, "a11y.menu_closed" ),
                enum_entry( EventKind::a11y_focus_changed, "a11y.focus_changed" ),
                enum_entry( EventKind::a11y_text_changed, "a11y.text_changed" ),
                enum_entry( EventKind::a11y_state_changed, "a11y.state_changed" ),
                enum_entry( EventKind::app_tab_changed, "app.tab_changed" ),
                enum_entry( EventKind::app_context_update, "app.context_update" ),
                enum_entry( EventKind::browser_tab_switched,
                            "browser.tab_switched" ),
                enum_entry( EventKind::state_snapshot, "state.snapshot" ),
            } ),
        };

        inline constexpr std::size_t kEventKindWireNameCount = 22U;
        static_assert( enum_table_has_count( kEventKindWireNames,
                                             kEventKindWireNameCount ) );

    }    // namespace detail

    // Fallback for an unmapped kind: the same "unspecified" sentinel the storage
    // sink has always written for kinds it did not recognise.
    inline constexpr std::string_view kUnspecifiedWireName = "unspecified";

    [[nodiscard]]
    constexpr std::string_view
    wire_name( EventKind kind ) noexcept
    {
        return detail::kEventKindWireNames.text_of( kind, kUnspecifiedWireName );
    }

    [[nodiscard]]
    constexpr std::optional<EventKind>
    wire_kind( std::string_view name ) noexcept
    {
        return detail::kEventKindWireNames.value_of( name );
    }

}    // namespace grab
