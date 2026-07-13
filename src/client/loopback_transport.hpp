#pragma once

#include "client/transport.hpp"

namespace grab
{

    class ActiveKindProbe;
    class EventBus;

}    // namespace grab

namespace grab::client
{

    class LoopbackTransport final : public Transport
    {
        public:

            explicit LoopbackTransport( grab::EventBus&              bus,
                                        const grab::ActiveKindProbe* probe =
                                            nullptr ) noexcept;

            [[nodiscard]]
            grab::Result<void>
            push_event( grab::Event event ) override;

            [[nodiscard]]
            grab::Result<SubscriptionHandle>
            subscribe( grab::EventFilter filter ) override;

            [[nodiscard]]
            grab::Result<std::vector<grab::EventTypeDescriptor>>
            list_event_types() override;

        private:

            grab::EventBus*              bus_   = nullptr;
            const grab::ActiveKindProbe* probe_ = nullptr;
    };

}    // namespace grab::client
