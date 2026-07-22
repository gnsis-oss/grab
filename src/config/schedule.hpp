#pragma once

#include "grab/config.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace grab::config
{

    enum class DueKind : std::uint8_t
    {
        Capture,
        Step,
        Idle,
        Count,
    };

    struct Due
    {
            DueKind                               kind{ DueKind::Idle };
            std::size_t                           step_index{};
            std::chrono::steady_clock::time_point wake_at;
    };

    class WatchSchedule
    {
        public:

            WatchSchedule( std::chrono::milliseconds interval,
                           const ScriptSection*      script );

            [[nodiscard]]
            Due
            next( std::chrono::steady_clock::time_point now );

            void
            capture_done( std::chrono::steady_clock::time_point now );

            void
            step_done( std::chrono::steady_clock::time_point now );

            void
            fail_script();

            [[nodiscard]]
            std::uint64_t
            skipped_captures() const noexcept;

        private:

            using TimePoint = std::chrono::steady_clock::time_point;

            void
            initialize( TimePoint now );

            void
            arm_step( TimePoint now );

            void
            account_for_overrun( TimePoint now );

        private:

            std::chrono::milliseconds interval_;
            const ScriptSection*      script_{};
            TimePoint                 capture_at_;
            TimePoint                 step_at_;
            std::size_t               step_index_{};
            std::uint64_t             skipped_captures_{};
            bool                      initialized_{};
            bool                      script_active_{};
    };

}    // namespace grab::config
