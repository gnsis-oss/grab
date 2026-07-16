#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/result.hpp"

#include <chrono>
#include <cstdint>

namespace grab::kernel::capture
{

    // Single source of truth for frame pacing: turns a target frame rate into
    // a fixed inter-frame interval and computes successive capture deadlines.
    class PacingGovernor final
    {
        public:

            [[nodiscard]]
            static grab::Result<PacingGovernor>
            for_fps( std::uint32_t frames_per_second );

            [[nodiscard]]
            std::chrono::nanoseconds
            interval() const noexcept;

            [[nodiscard]]
            std::chrono::steady_clock::time_point
            next_deadline( std::chrono::steady_clock::time_point from ) const noexcept;

        private:

            explicit PacingGovernor( std::chrono::nanoseconds interval ) noexcept;

            std::chrono::nanoseconds interval_;
    };

}    // namespace grab::kernel::capture
