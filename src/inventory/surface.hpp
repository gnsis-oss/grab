#pragma once

#include "grab/enum_table.hpp"
#include "inventory/action.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace grab::inventory
{

    inline constexpr std::string_view category_menu               = "menu-context";
    inline constexpr std::string_view category_dialog             = "dialog-wizard";
    inline constexpr std::string_view category_dock               = "dock-panel";

    inline constexpr std::string_view reachable_live_text         = "live";
    inline constexpr std::string_view reachable_best_effort_text  = "best-effort";
    inline constexpr std::string_view reachable_needs_data_text   = "needs-data";
    inline constexpr std::string_view reachable_needs_server_text = "needs-server";
    inline constexpr std::string_view reachable_unreachable_text  = "unreachable";

    enum class Reachable : std::uint8_t
    {
        live,
        best_effort,
        needs_data,
        needs_server,
        unreachable,
        count,
    };

    namespace detail
    {

        inline constexpr auto reachable_names = grab::EnumTable{
            std::to_array( {
                grab::enum_entry( Reachable::live, reachable_live_text ),
                grab::enum_entry( Reachable::best_effort, reachable_best_effort_text ),
                grab::enum_entry( Reachable::needs_data, reachable_needs_data_text ),
                grab::enum_entry( Reachable::needs_server, reachable_needs_server_text ),
                grab::enum_entry( Reachable::unreachable, reachable_unreachable_text ),
            } ),
        };
        static_assert( grab::enum_table_has_count( reachable_names,
                                                   Reachable::count ) );

    }    // namespace detail

    [[nodiscard]]
    constexpr std::string_view
    to_string( Reachable reachable ) noexcept
    {
        return detail::reachable_names.text_of( reachable, reachable_unreachable_text );
    }

    [[nodiscard]]
    constexpr std::optional<Reachable>
    reachable_from_string( std::string_view value ) noexcept
    {
        return detail::reachable_names.value_of( value );
    }

    struct Surface
    {
            std::string       name;
            std::string       output;
            std::string       category;
            std::string       module;
            Reachable         reachable = Reachable::unreachable;
            std::vector<Step> steps;
            std::string       skip_reason;
    };

    [[nodiscard]]
    inline bool
    attemptable( const Surface& surface ) noexcept
    {
        const auto reachable = surface.reachable ==
                               Reachable::live ||
                               surface.reachable == Reachable::best_effort;
        return reachable && !surface.steps.empty();
    }

}    // namespace grab::inventory
