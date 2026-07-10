#include <grpcpp/server.h>

#include "grab/event_bus.hpp"
#include "grab/result.hpp"
#include "transport/server.hpp"
#include "transport/service.hpp"

#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server_builder.h>
#include <memory>
#include <string>
#include <utility>

namespace grab::transport
{

    TransportServer::TransportServer( std::string                   endpoint,
                                      std::unique_ptr<EventService> service,
                                      std::unique_ptr<grpc::Server> server ) noexcept :
        endpoint_( std::move( endpoint ) ),
        service_( std::move( service ) ),
        server_( std::move( server ) )
    {
    }

    TransportServer::~TransportServer()
    {
        shutdown();
    }

    TransportServer::TransportServer( TransportServer&& other ) noexcept = default;

    TransportServer&
    TransportServer::operator=( TransportServer&& other ) noexcept
    {
        if( this != &other )
        {
            shutdown();
            endpoint_ = std::move( other.endpoint_ );
            service_  = std::move( other.service_ );
            server_   = std::move( other.server_ );
        }
        return *this;
    }

    grab::Result<TransportServer>
    TransportServer::start( const std::string& endpoint,
                            grab::EventBus&    bus )
    {
        auto                service = std::make_unique<EventService>( bus );
        grpc::ServerBuilder builder;
        builder.AddListeningPort( endpoint, grpc::InsecureServerCredentials() );
        builder.RegisterService( service.get() );

        auto server = builder.BuildAndStart();
        if( server == nullptr )
        {
            return grab::fail( grab::ErrorCode::ProtocolError,
                               "failed to start transport server at " + endpoint );
        }

        return TransportServer{ endpoint, std::move( service ), std::move( server ) };
    }

    void
    TransportServer::shutdown() noexcept
    {
        if( server_ == nullptr )
        {
            return;
        }

        server_->Shutdown();
        server_->Wait();
        server_.reset();
        service_.reset();
    }

    const std::string&
    TransportServer::endpoint() const noexcept
    {
        return endpoint_;
    }

}    // namespace grab::transport
