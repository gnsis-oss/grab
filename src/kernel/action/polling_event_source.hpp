#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/context.hpp"
#include "grab/result.hpp"
#include "spi/event_source.hpp"

#include <chrono>

namespace grab::kernel::action
{

    // An event source that carries no external events: wait_for_event paces the
    // WaitEngine's poll loop by blocking (interruptibly) up to the requested
    // budget, so no raw sleep is needed at the call site.
    class PollingEventSource final : public grab::spi::EventSource
    {
        public:

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
    };

}    // namespace grab::kernel::action
