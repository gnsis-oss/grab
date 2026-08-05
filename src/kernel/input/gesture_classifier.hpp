#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/event.hpp"
#include "grab/space.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace grab::kernel::input
{

    enum class Gesture : std::uint8_t
    {
        Move,
        Hold,
        HoldEnd,
        HoldCancel,
        Click,
        DoubleClick,
        Pause,
    };

    struct GestureEvent
    {
            Gesture       kind;
            SpacePoint    at;
            std::uint32_t button;
    };

    struct GestureThresholds
    {
            std::chrono::milliseconds hold{ 500 };
            std::chrono::milliseconds double_click{ 400 };
            std::chrono::milliseconds pause{ 700 };
            double                    slop_px = 5.0;
    };

    class GestureClassifier final
    {
        public:

            explicit GestureClassifier( GestureThresholds thresholds );

            // A missing optional pointer position is non-positional: it never
            // replaces a spatial anchor or synthesizes motion. A positionless
            // press claims first-button ownership but cannot start a spatial
            // gesture; its release clears that ownership. A positionless
            // release of a spatial gesture uses the stored press point.
            std::vector<GestureEvent>
            feed( const Event& event );

            std::vector<GestureEvent>
            advance( std::chrono::milliseconds now );

            void
            reset();

        private:

            enum class State : std::uint8_t
            {
                Idle,
                Moving,
                Pressed,
                Holding,
                AwaitDouble,
                PostDouble,
            };

            [[nodiscard]]
            std::vector<GestureEvent>
            process_deadline( std::chrono::milliseconds now );

            void
            handle_motion( const SpacePoint&          position,
                           std::chrono::milliseconds  now,
                           std::vector<GestureEvent>& output );

            void
            handle_button_down( std::uint32_t                    button,
                                const std::optional<SpacePoint>& position,
                                std::chrono::milliseconds        now,
                                std::vector<GestureEvent>&       output );

            void
            handle_button_up( std::uint32_t                    button,
                              const std::optional<SpacePoint>& position,
                              std::vector<GestureEvent>&       output );

            void
            begin_press( std::uint32_t             button,
                         const SpacePoint&         position,
                         std::chrono::milliseconds now );

            void
            begin_moving( const SpacePoint&         position,
                          std::chrono::milliseconds now );

            [[nodiscard]]
            bool
            mark_button_down( std::uint32_t button ) noexcept;

            [[nodiscard]]
            bool
            mark_button_up( std::uint32_t button ) noexcept;

            [[nodiscard]]
            bool
                              button_is_down( std::uint32_t button ) const noexcept;

            GestureThresholds thresholds_;
            State             state_{ State::Idle };
            SpacePoint        press_at_{};
            std::chrono::milliseconds press_time_{};
            SpacePoint                last_motion_at_{};
            std::chrono::milliseconds last_motion_time_{};
            std::uint32_t             owner_button_{};
            std::uint8_t              buttons_down_{};
    };

}    // namespace grab::kernel::input
