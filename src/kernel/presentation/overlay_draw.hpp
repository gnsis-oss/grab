#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/overlay.hpp"
#include "grab/space.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace grab::overlay
{

    inline constexpr double      minPathSampleSpacingPx = 2.0;
    inline constexpr std::size_t maxPathSamples         = 4'096U;
    inline constexpr double      minDrawExtentPx        = 3.0;
    inline constexpr float       defaultDrawStrokePx    = 3.0F;

    enum class DrawKind : std::uint8_t
    {
        Rectangle,
        Ellipse,
        Path,
    };

    struct DrawStyle
    {
            overlay::Color color     = overlay::defaultOverlayColor;
            float          stroke_px = defaultDrawStrokePx;
            bool           filled{};
    };

    class DrawInteraction
    {
        public:

            DrawInteraction()  = default;
            ~DrawInteraction() = default;

            void
            begin( DrawKind         kind,
                   SpacePoint       at,
                   const DrawStyle& style );

            [[nodiscard]]
            std::optional<overlay::Shape>
            update( SpacePoint at );

            [[nodiscard]]
            std::optional<overlay::Shape>
            commit( SpacePoint at );

            void
            cancel() noexcept;

            [[nodiscard]]
            bool
            active() const noexcept;

            [[nodiscard]]
            DrawKind
            kind() const noexcept;

        private:

            [[nodiscard]]
            bool
            can_sample( SpacePoint at ) const noexcept;

            void
            sample( SpacePoint at );

            [[nodiscard]]
            std::optional<overlay::Shape>
                                    current_shape() const;

            DrawKind                kind_{ DrawKind::Rectangle };
            DrawStyle               style_{};
            SpacePoint              began_at_{};
            SpacePoint              current_at_{};
            std::vector<SpacePoint> path_points_;
            bool                    active_{};
    };

}    // namespace grab::overlay
