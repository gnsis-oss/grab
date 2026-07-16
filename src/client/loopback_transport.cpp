#include "client/loopback_transport.hpp"
#include "client/transport.hpp"
#include "grab/capture.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/event_descriptor.hpp"
#include "grab/interaction.hpp"
#include "grab/locator.hpp"
#include "grab/query.hpp"
#include "grab/result.hpp"
#include "grab/session.hpp"
#include "grab/trace.hpp"

#include <expected>
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

    LoopbackTransport::LoopbackTransport( std::unique_ptr<grab::Session> session,
                                          const grab::ActiveKindProbe* probe ) noexcept :
        session_( std::move( session ) ),
        probe_( probe )
    {
    }

    grab::Result<grab::Match>
    LoopbackTransport::resolve( const grab::Locator& locator,
                                grab::Cardinality    cardinality )
    {
        if( !session_ )
        {
            return grab::fail( grab::ErrorCode::CapabilityUnavailable,
                               "loopback transport has no bound session" );
        }

        return session_->resolve( locator, cardinality );
    }

    grab::Result<grab::Receipt>
    LoopbackTransport::perform( const grab::Action&        action,
                                const grab::ActionOptions& options )
    {
        if( !session_ )
        {
            return grab::fail( grab::ErrorCode::CapabilityUnavailable,
                               "loopback transport has no bound session" );
        }

        return session_->perform( action, options );
    }

    grab::Result<grab::Frame>
    LoopbackTransport::capture( const grab::CaptureTarget&  target,
                                const grab::CaptureOptions& options )
    {
        if( !session_ )
        {
            return grab::fail( grab::ErrorCode::CapabilityUnavailable,
                               "loopback transport has no bound session" );
        }

        return session_->capture( target, options );
    }

    grab::Result<void>
    LoopbackTransport::push_event( grab::Event event )
    {
        if( !bus_ )
        {
            return grab::fail( grab::ErrorCode::CapabilityUnavailable,
                               "loopback transport has no event bus" );
        }

        bus_->publish( std::move( event ) );
        return {};
    }

    grab::Result<SubscriptionHandle>
    LoopbackTransport::subscribe( grab::EventFilter filter )
    {
        if( bus_ )
        {
            SubscriptionHandle stream = std::make_unique<LoopbackSubscriptionStream>(
                bus_->subscribe( std::move( filter ) )
            );
            return stream;
        }

        if( session_ )
        {
            auto subscription = session_->watch( grab::SubscriptionScope{
                .kinds  = {},
                .filter = std::move( filter ),
            } );
            if( !subscription.has_value() )
            {
                return std::unexpected( std::move( subscription.error() ) );
            }

            SubscriptionHandle stream = std::make_unique<LoopbackSubscriptionStream>(
                std::move( *subscription )
            );
            return stream;
        }

        return grab::fail( grab::ErrorCode::CapabilityUnavailable,
                           "loopback transport has no event bus" );
    }

    grab::Result<std::vector<grab::EventTypeDescriptor>>
    LoopbackTransport::list_event_types()
    {
        return grab::event_type_descriptors( probe_ );
    }

}    // namespace grab::client
