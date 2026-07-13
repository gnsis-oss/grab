#include "client/client.hpp"
#include "client/transport.hpp"
#include "grab/event.hpp"
#include "grab/event_descriptor.hpp"
#include "grab/result.hpp"

#include <expected>
#include <memory>
#include <utility>
#include <vector>

namespace grab::client
{

    Client::Client( Transport& transport ) noexcept :
        transport_( &transport )
    {
    }

    Client::Client( std::unique_ptr<Transport> transport ) noexcept :
        owned_transport_( std::move( transport ) ),
        transport_( owned_transport_.get() )
    {
    }

    grab::Result<Transport*>
    Client::transport() noexcept
    {
        if( transport_ == nullptr )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "client transport must not be null" );
        }
        return transport_;
    }

    grab::Result<void>
    Client::push_event( grab::Event event )
    {
        auto bound_transport = transport();
        if( !bound_transport.has_value() )
        {
            return std::unexpected( bound_transport.error() );
        }
        return ( *bound_transport )->push_event( std::move( event ) );
    }

    grab::Result<SubscriptionHandle>
    Client::subscribe( grab::EventFilter filter )
    {
        auto bound_transport = transport();
        if( !bound_transport.has_value() )
        {
            return std::unexpected( bound_transport.error() );
        }
        return ( *bound_transport )->subscribe( std::move( filter ) );
    }

    grab::Result<std::vector<grab::EventTypeDescriptor>>
    Client::list_event_types()
    {
        auto bound_transport = transport();
        if( !bound_transport.has_value() )
        {
            return std::unexpected( bound_transport.error() );
        }
        return ( *bound_transport )->list_event_types();
    }

}    // namespace grab::client
