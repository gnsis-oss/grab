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
        // give the new kind a row here at the same time. eventKindWireNameCount
        // then guards against an accidentally dropped or duplicated row.
        inline constexpr auto eventKindWireNames = EnumTable{
            std::to_array( {
                enum_entry( EventKind::Unspecified, "unspecified" ),
                enum_entry( EventKind::KeyDown, "input.key_down" ),
                enum_entry( EventKind::KeyUp, "input.key_up" ),
                enum_entry( EventKind::KeyCombo, "input.key_combo" ),
                enum_entry( EventKind::MouseClick, "input.mouse_click" ),
                enum_entry( EventKind::MouseMove, "input.mouse_move" ),
                enum_entry( EventKind::IdleStart, "input.idle_start" ),
                enum_entry( EventKind::IdleEnd, "input.idle_end" ),
                enum_entry( EventKind::WindowFocusChanged, "window.focus_changed" ),
                enum_entry( EventKind::WindowTitleChanged, "window.title_changed" ),
                enum_entry( EventKind::WindowCreated, "window.created" ),
                enum_entry( EventKind::WindowClosed, "window.closed" ),
                enum_entry( EventKind::A11yButtonClicked, "a11y.button_clicked" ),
                enum_entry( EventKind::A11yMenuOpened, "a11y.menu_opened" ),
                enum_entry( EventKind::A11yMenuClosed, "a11y.menu_closed" ),
                enum_entry( EventKind::A11yFocusChanged, "a11y.focus_changed" ),
                enum_entry( EventKind::A11yTextChanged, "a11y.text_changed" ),
                enum_entry( EventKind::A11yStateChanged, "a11y.state_changed" ),
                enum_entry( EventKind::AppTabChanged, "app.tab_changed" ),
                enum_entry( EventKind::AppContextUpdate, "app.context_update" ),
                enum_entry( EventKind::BrowserTabSwitched, "browser.tab_switched" ),
                enum_entry( EventKind::StateSnapshot, "state.snapshot" ),
            } ),
        };

        inline constexpr std::size_t eventKindWireNameCount = 22U;
        static_assert( enum_table_has_count( eventKindWireNames,
                                             eventKindWireNameCount ) );

    }    // namespace detail

    // Fallback for an unmapped kind: the same "unspecified" sentinel the storage
    // sink has always written for kinds it did not recognise.
    inline constexpr std::string_view unspecifiedWireName = "unspecified";

    [[nodiscard]]
    constexpr std::string_view
    wire_name( EventKind kind ) noexcept
    {
        return detail::eventKindWireNames.text_of( kind, unspecifiedWireName );
    }

    [[nodiscard]]
    constexpr std::optional<EventKind>
    wire_kind( std::string_view name ) noexcept
    {
        return detail::eventKindWireNames.value_of( name );
    }

}    // namespace grab
