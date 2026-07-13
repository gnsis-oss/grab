#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/geometry/rectangle.hpp"
#include "kernel/capture/tile_differ.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace grab::kernel
{

    enum class DamageHintAction : std::uint8_t
    {
        UseHints,
        FullScan,
        FullInvalidation,
    };

    struct DamageHintAudit
    {
            DamageHintAction action{ DamageHintAction::UseHints };
            double           miss_rate{};
            std::string      reason;
    };

    class DamageHintAuditor final
    {
        public:

            explicit DamageHintAuditor( double miss_threshold = 0.20 );

            [[nodiscard]]
            DamageHintAudit
            audit( std::span<const geometry::Rectangle> hints,
                   const TileDiffResult&                ground_truth,
                   bool                                 overflowed = false );

            [[nodiscard]]
            bool
            degraded() const noexcept;

            [[nodiscard]]
            std::string_view
            degradation_reason() const noexcept;

        private:

            double      miss_threshold_{};
            bool        degraded_{};
            std::string degradation_reason_;
    };

}    // namespace grab::kernel
