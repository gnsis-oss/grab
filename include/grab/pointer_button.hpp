#pragma once

#include <cstdint>

namespace grab::input
{

    enum class PointerButton : std::uint8_t
    {
        Primary   = 1U,
        Middle    = 2U,
        Secondary = 3U,
    };

    [[nodiscard]]
    constexpr std::uint8_t
    button_code( PointerButton button ) noexcept
    {
        return static_cast<std::uint8_t>( button );
    }

    inline constexpr std::uint8_t primaryButton = button_code( PointerButton::Primary );

}    // namespace grab::input
