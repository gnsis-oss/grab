#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/origin.hpp"
#include "grab/overlay.hpp"
#include "grab/space.hpp"
#include "grab/watch.hpp"

#include <chrono>
#include <cstdint>
#include <optional>

namespace grab::kernel::presentation
{

    inline constexpr overlay::Color defaultPhysicalTrailColor =
        overlay::defaultOverlayColor;
    inline constexpr overlay::Color defaultInjectedTrailColor =
        overlay::defaultOverlayColor;
    inline constexpr std::chrono::milliseconds defaultTrailFade{ 1'200 };
    inline constexpr float                     defaultTrailWidthPx = 3.0F;
    inline constexpr std::chrono::milliseconds trailBreakInterval{ 250 };
    inline constexpr double                    trailBreakDistancePx = 160.0;

    struct TrailStyle
    {
            overlay::Color            physical{ defaultPhysicalTrailColor };
            overlay::Color            injected{ defaultInjectedTrailColor };
            std::chrono::milliseconds fade{ defaultTrailFade };
            float                     width_px = defaultTrailWidthPx;
    };

    class OverlayScene;

    class TrailAnimator final
    {
        public:

            TrailAnimator( OverlayScene& scene,
                           TrailStyle    style );

            void
            consume( const SubscriptionEvent& item );

            [[nodiscard]]
            std::uint64_t
            scene_add_failure_count() const noexcept;

        private:

            struct Sample
            {
                    SpacePoint  position;
                    EventOrigin origin{ EventOrigin::Unknown };
                    double      timestamp{};
            };

            void
                                  break_path() noexcept;

            OverlayScene&         scene_;
            TrailStyle            style_;
            std::optional<Sample> previous_;
            std::uint64_t         scene_add_failures_{};
    };

}    // namespace grab::kernel::presentation
