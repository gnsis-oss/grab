#pragma once

#include <chrono>
#include <cstdint>
#include <limits>

namespace grab::input
{

    struct DragOptions
    {
            enum class Path : std::uint8_t
            {
                Linear,
                Cubic,
            };

            static constexpr std::int32_t minimumInterpolationSteps = 1;
            static constexpr std::int32_t maximumInterpolationSteps =
                std::numeric_limits<std::uint16_t>::max();
            static constexpr std::int32_t defaultInterpolationSteps = 16;
            static constexpr auto     defaultStepDwell = std::chrono::milliseconds{ 8 };

            std::int32_t              interpolation_steps = defaultInterpolationSteps;
            std::chrono::milliseconds step_dwell          = defaultStepDwell;
            Path                      path                = Path::Linear;
    };

}    // namespace grab::input
