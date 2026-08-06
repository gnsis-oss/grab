#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

// Seat double for tests. execute_drag and the command executor are templated on
// SeatT, so this satisfies them without an X connection.

#include "grab/result.hpp"

#include <chrono>
#include <cstdint>
#include <vector>

namespace grab::testing
{

    struct SeatEvent
    {
            enum class Kind : std::uint8_t
            {
                Move,
                Button,
                Flush,
                Count,
            };

            Kind                                  kind{ Kind::Flush };
            std::int16_t                          x{};
            std::int16_t                          y{};
            std::uint8_t                          button{};
            bool                                  pressed{};
            std::chrono::steady_clock::time_point at{};
    };

    class RecordingSeat final
    {
        public:

            [[nodiscard]]
            grab::Result<void>
            move_pointer_absolute( std::int16_t x,
                                   std::int16_t y )
            {
                events_.push_back( SeatEvent{
                    .kind = SeatEvent::Kind::Move,
                    .x    = x,
                    .y    = y,
                    .at   = now_
                } );
                return {};
            }

            [[nodiscard]]
            grab::Result<void>
            button( std::uint8_t code,
                    bool         pressed )
            {
                events_.push_back( SeatEvent{
                    .kind    = SeatEvent::Kind::Button,
                    .button  = code,
                    .pressed = pressed,
                    .at      = now_
                } );
                return {};
            }

            [[nodiscard]]
            grab::Result<void>
            flush()
            {
                events_.push_back(
                    SeatEvent{ .kind = SeatEvent::Kind::Flush, .at = now_ }
                );
                return {};
            }

            void
            set_now( std::chrono::steady_clock::time_point now ) noexcept
            {
                now_ = now;
            }

            [[nodiscard]]
            const std::vector<SeatEvent>&
            events() const noexcept
            {
                return events_;
            }

        private:

            std::vector<SeatEvent>                events_;
            std::chrono::steady_clock::time_point now_{};
    };

}    // namespace grab::testing
