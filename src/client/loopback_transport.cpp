#include "client/loopback_transport.hpp"
#include "client/transport.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/event_descriptor.hpp"
#include "grab/result.hpp"

#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace grab::client
{
    namespace
    {

        class LoopbackSubscriptionStream final : public SubscriptionStream
        {
            public:

                explicit LoopbackSubscriptionStream(
                    grab::Subscription subscription
                ) noexcept :
                    subscription_( std::move( subscription ) )
                {
                }

                [[nodiscard]]
                grab::Result<std::optional<grab::SubscriptionEvent>>
                try_next() override
                {
                    return subscription_.try_pop_item();
                }

            private:

                grab::Subscription subscription_;
        };

    }    // namespace

    LoopbackTransport::LoopbackTransport( grab::EventBus&              bus,
                                          const grab::ActiveKindProbe* probe ) noexcept :
        bus_( &bus ),
        probe_( probe )
    {
    }

    grab::Result<void>
    LoopbackTransport::push_event( grab::Event event )
    {
        bus_->publish( std::move( event ) );
        return {};
    }

    grab::Result<SubscriptionHandle>
    LoopbackTransport::subscribe( grab::EventFilter filter )
    {
        SubscriptionHandle stream = std::make_unique<LoopbackSubscriptionStream>(
            bus_->subscribe( std::move( filter ) )
        );
        return stream;
    }

    grab::Result<std::vector<grab::EventTypeDescriptor>>
    LoopbackTransport::list_event_types()
    {
        return grab::event_type_descriptors( probe_ );
    }

}    // namespace grab::client
