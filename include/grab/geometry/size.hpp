#pragma once

#include <cstdint>

namespace grab::geometry
{

    struct Size
    {
            std::uint32_t width  = 0U;
            std::uint32_t height = 0U;

            [[nodiscard]]
            constexpr std::uint32_t
            area() const noexcept
            {
                return width * height;
            }

            [[nodiscard]]
            friend constexpr bool
            operator==( Size lhs,
                        Size rhs ) noexcept = default;
    };

}    // namespace grab::geometry
