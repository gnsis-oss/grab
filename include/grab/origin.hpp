#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/enum_table.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace grab
{

    enum class EventOrigin : std::uint8_t
    {
        Physical,
        InjectedSelf,
        InjectedOther,
        Unknown,
    };

    namespace detail
    {

        inline constexpr std::size_t eventOriginCount  = 4U;
        inline constexpr auto        event_origin_name = EnumTable{
            std::to_array( {
                enum_entry( EventOrigin::Physical, "physical" ),
                enum_entry( EventOrigin::InjectedSelf, "injected_self" ),
                enum_entry( EventOrigin::InjectedOther, "injected_other" ),
                enum_entry( EventOrigin::Unknown, "unknown" ),
            } ),
        };
        static_assert( enum_table_has_count( event_origin_name,
                                             eventOriginCount ) );

    }    // namespace detail

}    // namespace grab
