#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "drivers/desktop/x11/injection_ledger.hpp"
#include "grab/event.hpp"
#include "spi/event_source.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
#include <xcb/xcb.h>

namespace grab::drivers::desktop::x11
{

    class X11EventSource final : public grab::spi::EventSource
    {
        public:

            using EventSink = std::function<void( grab::Event&& )>;

            [[nodiscard]]
            static grab::Result<std::unique_ptr<X11EventSource>>
            open( xcb_connection_t* connection,
                  xcb_window_t      root,
                  InjectionLedger&  ledger );

            void
            set_sink( EventSink sink ) override;

            [[nodiscard]]
            grab::Result<void>
            enable( const grab::spi::EventSpec& spec ) override;

            [[nodiscard]]
            grab::Result<void>
            disable( const grab::spi::EventSpec& spec ) override;

            [[nodiscard]]
            grab::Result<void>
            wait_for_event( const grab::spi::EventSpec&   spec,
                            const grab::OperationContext& context,
                            std::chrono::nanoseconds      maximum_wait ) override;

        private:

            static constexpr std::size_t inputDemandCount = 4U;

            X11EventSource( xcb_connection_t*          connection,
                            xcb_window_t               root,
                            std::uint8_t               extension_opcode,
                            std::vector<std::uint16_t> xtest_device_ids,
                            InjectionLedger&           ledger ) noexcept;

            [[nodiscard]]
            std::uint32_t
            active_mask() const noexcept;

            [[nodiscard]]
            grab::Result<void>
                                       select_events( std::uint32_t mask );

            xcb_connection_t*          connection_{};
            xcb_window_t               root_{};
            std::uint8_t               extension_opcode_{};
            std::vector<std::uint16_t> xtest_device_ids_;
            InjectionLedger*           ledger_{};
            std::array<std::size_t, inputDemandCount> demand_refcounts_{};
            std::mutex                                state_mutex_;
            EventSink                                 sink_;
            std::mutex                                sink_mutex_;
    };

}    // namespace grab::drivers::desktop::x11
