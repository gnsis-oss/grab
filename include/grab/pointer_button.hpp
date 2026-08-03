#pragma once

#include <cstdint>

namespace grab::input
{

    // X11 delivers wheel motion as button events, so the wheel directions are
    // buttons here too rather than a separate axis type. Naming them keeps
    // callers from writing the bare codes 4-7, which is what scrolling looked
    // like before Input::scroll existed.
    enum class PointerButton : std::uint8_t
    {
        Primary    = 1U,
        Middle     = 2U,
        Secondary  = 3U,
        WheelUp    = 4U,
        WheelDown  = 5U,
        WheelLeft  = 6U,
        WheelRight = 7U,
    };

    [[nodiscard]]
    constexpr std::uint8_t
    button_code( PointerButton button ) noexcept
    {
        return static_cast<std::uint8_t>( button );
    }

    inline constexpr std::uint8_t primaryButton = button_code( PointerButton::Primary );

}    // namespace grab::input
