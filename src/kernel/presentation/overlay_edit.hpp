#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/geometry/rectangle.hpp"
#include "grab/overlay.hpp"
#include "grab/space.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace grab::kernel::presentation
{

    inline constexpr double      handle_px        = 8.0;
    inline constexpr double      hit_tolerance_px = 3.0;
    inline constexpr double      min_size_px      = 8.0;
    inline constexpr std::size_t max_region_rects = 100U;

    struct EditGeometryOptions
    {
            double handle_px        = grab::kernel::presentation::handle_px;
            double hit_tolerance_px = grab::kernel::presentation::hit_tolerance_px;
            double min_size_px      = grab::kernel::presentation::min_size_px;
    };

    [[nodiscard]]
    std::optional<overlay::ShapeId>
    hit_test( std::span<const overlay::ShapeRecord> shapes,
              std::span<const overlay::ShapeId>     editable,
              SpacePoint                            at,
              const EditGeometryOptions&            options );

    [[nodiscard]]
    std::vector<geometry::Rectangle>
    edit_input_region( std::span<const overlay::ShapeRecord> shapes,
                       std::span<const overlay::ShapeId>     editable,
                       const EditGeometryOptions&            options );

    class EditInteraction final
    {
        public:

            [[nodiscard]]
            bool
            begin( std::span<const overlay::ShapeRecord> shapes,
                   std::span<const overlay::ShapeId>     editable,
                   SpacePoint                            at,
                   const EditGeometryOptions&            options );

            [[nodiscard]]
            std::optional<overlay::Shape>
            update( SpacePoint at );

            [[nodiscard]]
            std::optional<overlay::Shape>
            commit( SpacePoint at );

            void
            cancel();

            [[nodiscard]]
            bool
            active() const noexcept;

            [[nodiscard]]
            overlay::ShapeId
            target() const noexcept;

        private:

            enum class ResizeHandle : std::uint8_t
            {
                None,
                TopLeft,
                Top,
                TopRight,
                Right,
                BottomRight,
                Bottom,
                BottomLeft,
                Left,
            };

            overlay::Shape      original_{};
            overlay::ShapeId    target_{};
            SpacePoint          began_at_{};
            EditGeometryOptions options_{};
            ResizeHandle        handle_{ ResizeHandle::None };
            bool                active_{};
    };

}    // namespace grab::kernel::presentation
