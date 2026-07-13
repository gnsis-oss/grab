#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/enum_table.hpp"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace grab
{

    struct RoleId
    {
            std::uint32_t value{};
            friend auto
            operator<=>( const RoleId&,
                         const RoleId& ) = default;
    };

    namespace role
    {

        inline constexpr RoleId        unknown{ 0U };
        inline constexpr RoleId        application{ 1U };
        inline constexpr RoleId        window{ 2U };
        inline constexpr RoleId        document{ 3U };
        inline constexpr RoleId        dialog{ 4U };
        inline constexpr RoleId        panel{ 5U };
        inline constexpr RoleId        tab{ 6U };
        inline constexpr RoleId        region{ 7U };
        inline constexpr RoleId        popup{ 8U };
        inline constexpr RoleId        menu{ 9U };
        inline constexpr RoleId        control{ 10U };
        inline constexpr RoleId        text{ 11U };
        inline constexpr RoleId        image{ 12U };
        inline constexpr RoleId        list{ 13U };
        inline constexpr RoleId        table{ 14U };

        inline constexpr std::size_t   coreCount = 15U;

        // Values at or above this boundary are reserved for namespaced
        // extensions and never overlap grab's closed core role table.
        inline constexpr std::uint32_t extensionBase = 0X80'00'00'00U;

    }    // namespace role

    namespace detail
    {

        inline constexpr auto roleNames = EnumTable{
            std::to_array( {
                enum_entry( role::unknown, "unknown" ),
                enum_entry( role::application, "application" ),
                enum_entry( role::window, "window" ),
                enum_entry( role::document, "document" ),
                enum_entry( role::dialog, "dialog" ),
                enum_entry( role::panel, "panel" ),
                enum_entry( role::tab, "tab" ),
                enum_entry( role::region, "region" ),
                enum_entry( role::popup, "popup" ),
                enum_entry( role::menu, "menu" ),
                enum_entry( role::control, "control" ),
                enum_entry( role::text, "text" ),
                enum_entry( role::image, "image" ),
                enum_entry( role::list, "list" ),
                enum_entry( role::table, "table" ),
            } ),
        };
        static_assert( enum_table_has_count( roleNames,
                                             role::coreCount ) );

    }    // namespace detail

    [[nodiscard]]
    constexpr std::string_view
    role_name( RoleId role_id ) noexcept
    {
        return detail::roleNames.text_of( role_id, "unknown" );
    }

}    // namespace grab
