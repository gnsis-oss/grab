#pragma once

#include "grab/enum_table.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace grab
{

    enum class PayloadField : std::uint8_t
    {
        KeyCode,
        KeyName,
        Text,
        Button,
        ButtonName,
        Axis,
        Delta,
        IdleSeconds,
        App,
        Pid,
        Title,
        PrevTitle,
        DurationSeconds,
        Role,
        Name,
        Detail,
        State,
        Json,
        TabTitle,
        PrevTabTitle,
        Count,
    };

    namespace detail
    {

        inline constexpr auto payloadFieldNames = EnumTable{
            std::to_array( {
                enum_entry( PayloadField::KeyCode, "key_code" ),
                enum_entry( PayloadField::KeyName, "key_name" ),
                enum_entry( PayloadField::Text, "text" ),
                enum_entry( PayloadField::Button, "button" ),
                enum_entry( PayloadField::ButtonName, "button_name" ),
                enum_entry( PayloadField::Axis, "axis" ),
                enum_entry( PayloadField::Delta, "delta" ),
                enum_entry( PayloadField::IdleSeconds, "idle_s" ),
                enum_entry( PayloadField::App, "app" ),
                enum_entry( PayloadField::Pid, "pid" ),
                enum_entry( PayloadField::Title, "title" ),
                enum_entry( PayloadField::PrevTitle, "prev_title" ),
                enum_entry( PayloadField::DurationSeconds, "duration_s" ),
                enum_entry( PayloadField::Role, "role" ),
                enum_entry( PayloadField::Name, "name" ),
                enum_entry( PayloadField::Detail, "detail" ),
                enum_entry( PayloadField::State, "state" ),
                enum_entry( PayloadField::Json, "json" ),
                enum_entry( PayloadField::TabTitle, "tab_title" ),
                enum_entry( PayloadField::PrevTabTitle, "prev_tab_title" ),
            } ),
        };
        static_assert( enum_table_has_count( payloadFieldNames,
                                             PayloadField::Count ) );

    }    // namespace detail

    [[nodiscard]]
    constexpr std::string_view
    field_name( PayloadField field ) noexcept
    {
        return detail::payloadFieldNames.text_of( field, "" );
    }

}    // namespace grab
