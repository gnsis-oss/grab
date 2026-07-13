#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/enum_table.hpp"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace grab
{

    struct RelationId
    {
            std::uint32_t value{};
            friend auto
            operator<=>( const RelationId&,
                         const RelationId& ) = default;
    };

    namespace relation
    {

        inline constexpr RelationId    contains{ 0U };
        inline constexpr RelationId    owns{ 1U };
        inline constexpr RelationId    presented_on{ 2U };
        inline constexpr RelationId    occupies{ 3U };
        inline constexpr RelationId    overlays{ 4U };
        inline constexpr RelationId    active_child{ 5U };
        inline constexpr RelationId    focus_within{ 6U };
        inline constexpr RelationId    embeds{ 7U };
        inline constexpr RelationId    controls{ 8U };
        inline constexpr RelationId    label_for{ 9U };
        inline constexpr RelationId    labelled_by{ 10U };
        inline constexpr RelationId    controlled_by{ 11U };
        inline constexpr RelationId    popup_for{ 12U };
        inline constexpr RelationId    flows_to{ 13U };

        inline constexpr std::size_t   coreCount    = 14U;
        inline constexpr std::uint32_t coreBitCount = 32U;

        // The closed core table currently occupies ids 0..13. Slots 14..31
        // are reserved for future core relations so every core kind remains
        // representable in one uint32_t source/target-edge mask. Namespaced
        // extension relations start at 32.
        inline constexpr std::uint32_t extensionBase = coreBitCount;

    }    // namespace relation

    namespace detail
    {

        inline constexpr auto relationNames = EnumTable{
            std::to_array( {
                enum_entry( relation::contains, "contains" ),
                enum_entry( relation::owns, "owns" ),
                enum_entry( relation::presented_on, "presented_on" ),
                enum_entry( relation::occupies, "occupies" ),
                enum_entry( relation::overlays, "overlays" ),
                enum_entry( relation::active_child, "active_child" ),
                enum_entry( relation::focus_within, "focus_within" ),
                enum_entry( relation::embeds, "embeds" ),
                enum_entry( relation::controls, "controls" ),
                enum_entry( relation::label_for, "label_for" ),
                enum_entry( relation::labelled_by, "labelled_by" ),
                enum_entry( relation::controlled_by, "controlled_by" ),
                enum_entry( relation::popup_for, "popup_for" ),
                enum_entry( relation::flows_to, "flows_to" ),
            } ),
        };
        static_assert( enum_table_has_count( relationNames,
                                             relation::coreCount ) );
        static_assert( relation::flows_to.value < relation::coreBitCount );

    }    // namespace detail

    [[nodiscard]]
    constexpr std::string_view
    relation_name( RelationId relation_id ) noexcept
    {
        return detail::relationNames.text_of( relation_id, "unknown" );
    }

    [[nodiscard]]
    constexpr bool
    is_core_relation( RelationId relation_id ) noexcept
    {
        return relation_id.value < relation::coreCount;
    }

}    // namespace grab
