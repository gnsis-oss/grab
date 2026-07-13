#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/geometry/rectangle.hpp"
#include "grab/ids.hpp"
#include "grab/result.hpp"
#include "grab/space.hpp"
#include "screen/enumerate.hpp"

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace grab::detail
{

    class SpaceGraph;

}

namespace grab::drivers::desktop::x11
{

    struct OutputSpace
    {
            std::string               name;
            grab::geometry::Rectangle bounds;
            grab::CoordinateSpaceId   space{};
            double                    scale{ 1.0 };
            grab::CaptureGeneration   generation{};
    };

    class CoordinateAuthority final
    {
        public:

            explicit CoordinateAuthority( const char* display = nullptr );

            [[nodiscard]]
            grab::Result<void>
            refresh();

            // Explicit topology input keeps graph behavior testable without a live
            // display. Production callers use the zero-argument refresh above.
            [[nodiscard]]
            grab::Result<void>
            refresh( std::span<const grab::screen::OutputInfo> outputs );

            [[nodiscard]]
            std::shared_ptr<const grab::detail::SpaceGraph>
            graph() const noexcept;

            [[nodiscard]]
            grab::CoordinateSpaceId
            global_space() const noexcept;

            [[nodiscard]]
            grab::DisplayGeneration
            generation() const noexcept;

            [[nodiscard]]
            grab::CaptureGeneration
            capture_generation() const noexcept;

            [[nodiscard]]
            grab::Result<OutputSpace>
            output_space( std::string_view name ) const;

            [[nodiscard]]
            std::span<const OutputSpace>
            output_spaces() const noexcept;

        private:

            [[nodiscard]]
            bool
            topology_matches( std::span<const grab::screen::OutputInfo> outputs ) const;

            std::string                               display_;
            bool                                      use_default_display_{ true };
            std::shared_ptr<grab::detail::SpaceGraph> graph_;
            grab::CoordinateSpaceId                   global_space_{};
            grab::DisplayGeneration                   generation_{};
            std::vector<OutputSpace>                  outputs_;
    };

}    // namespace grab::drivers::desktop::x11
