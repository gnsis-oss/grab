#pragma once

#include "client/transport.hpp"
#include "grab/session.hpp"

#include <memory>

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

            explicit LoopbackTransport( std::unique_ptr<grab::Session> session,
                                        const grab::ActiveKindProbe*   probe =
                                            nullptr ) noexcept;

            [[nodiscard]]
            grab::Result<grab::Match>
            resolve( const grab::Locator& locator,
                     grab::Cardinality    cardinality ) override;

            [[nodiscard]]
            grab::Result<grab::Receipt>
            perform( const grab::Action&        action,
                     const grab::ActionOptions& options ) override;

            [[nodiscard]]
            grab::Result<grab::Frame>
            capture( const grab::CaptureTarget&  target,
                     const grab::CaptureOptions& options ) override;

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

            grab::EventBus*                bus_ = nullptr;
            std::unique_ptr<grab::Session> session_;
            const grab::ActiveKindProbe*   probe_ = nullptr;
    };

}    // namespace grab::client
