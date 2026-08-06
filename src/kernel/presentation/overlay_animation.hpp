#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/overlay.hpp"

#include <chrono>
#include <optional>

namespace grab::kernel::presentation
{

    struct EvaluatedReveal
    {
            overlay::Axis axis      = overlay::Axis::X;
            overlay::Edge from_edge = overlay::Edge::Min;
            double        fraction  = 1.0;
    };

    struct EvaluatedAnimation
    {
            double                         scale       = 1.0;
            double                         opacity     = 1.0;
            double                         translate_x = 0.0;
            double                         translate_y = 0.0;
            std::optional<EvaluatedReveal> reveal;
            std::chrono::milliseconds      duration{};
            bool                           complete = true;
    };

    struct AnimationRect
    {
            double x{};
            double y{};
            double width{};
            double height{};
    };

    [[nodiscard]]
    bool
    valid_animation( const overlay::AnimationSpec& animation ) noexcept;

    [[nodiscard]]
    std::chrono::milliseconds
    animation_duration( const overlay::AnimationSpec& animation ) noexcept;

    [[nodiscard]]
    EvaluatedAnimation
    evaluate_animation( const overlay::AnimationSpec& animation,
                        std::chrono::milliseconds     elapsed ) noexcept;

    [[nodiscard]]
    EvaluatedAnimation
    evaluate_animation( const overlay::ShapeRecord& record,
                        std::chrono::milliseconds   now ) noexcept;

    [[nodiscard]]
    double
    evaluate_opacity( const overlay::ShapeRecord& record,
                      std::chrono::milliseconds   now ) noexcept;

    [[nodiscard]]
    AnimationRect
    reveal_clip( AnimationRect          bounds,
                 const EvaluatedReveal& reveal ) noexcept;

}    // namespace grab::kernel::presentation
