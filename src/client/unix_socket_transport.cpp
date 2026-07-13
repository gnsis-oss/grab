#include "client/transport.hpp"
#include "client/unix_socket_transport.hpp"
#include "eventgrab/v1/events.pb.h"
#include "eventgrab/v1/service.grpc.pb.h"
#include "eventgrab/v1/service.pb.h"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/event_descriptor.hpp"
#include "grab/result.hpp"
#include "grab/trace.hpp"
#include "transport/codec.hpp"
#include "transport/proto_descriptor.hpp"

#include <chrono>
#include <cstddef>
#include <deque>
#include <expected>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/support/status.h>
#include <grpcpp/support/status_code_enum.h>
#include <grpcpp/support/sync_stream.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace grab::client
{
    namespace
    {

        inline constexpr auto rpcTimeout = std::chrono::seconds{ 2 };

        [[nodiscard]]
        auto
        rpc_deadline()
        {
            return std::chrono::system_clock::now() + rpcTimeout;
        }

        [[nodiscard]]
        grab::Error
        grpc_error( const grpc::Status& status )
        {
            grab::ErrorCode        code        = grab::ErrorCode::ProtocolError;
            grab::ErrorDisposition disposition = grab::ErrorDisposition::Fatal;

            switch( status.error_code() )
            {
                case grpc::StatusCode::UNAVAILABLE :
                    code        = grab::ErrorCode::EnvironmentChanged;
                    disposition = grab::ErrorDisposition::RetrySame;
                    break;
                case grpc::StatusCode::DEADLINE_EXCEEDED :
                    code        = grab::ErrorCode::DeadlineExceeded;
                    disposition = grab::ErrorDisposition::RetrySame;
                    break;
                case grpc::StatusCode::CANCELLED :
                    code        = grab::ErrorCode::Cancelled;
                    disposition = grab::ErrorDisposition::RetrySame;
                    break;
                case grpc::StatusCode::INVALID_ARGUMENT :
                    code = grab::ErrorCode::InvalidArgument;
                    break;
                case grpc::StatusCode::UNAUTHENTICATED :
                case grpc::StatusCode::PERMISSION_DENIED :
                    code = grab::ErrorCode::PermissionDenied;
                    break;
                case grpc::StatusCode::NOT_FOUND :
                    code = grab::ErrorCode::SessionNotFound;
                    break;
                case grpc::StatusCode::ALREADY_EXISTS :
                    code = grab::ErrorCode::SessionExists;
                    break;
                case grpc::StatusCode::RESOURCE_EXHAUSTED :
                    code = grab::ErrorCode::Overflowed;
                    break;
                case grpc::StatusCode::UNIMPLEMENTED :
                    code = grab::ErrorCode::CapabilityUnavailable;
                    break;
                case grpc::StatusCode::OK :
                    code = grab::ErrorCode::InternalFault;
                    break;
                default :
                    break;
            }

            auto message = status.error_message();
            if( message.empty() )
            {
                message = "gRPC transport request failed";
            }
            return grab::Error{
                .code        = code,
                .message     = std::move( message ),
                .capability  = {},
                .target      = {},
                .attempts    = {},
                .disposition = disposition,
                .diagnostics = {},
            };
        }

        [[nodiscard]]
        eventgrab::v1::EventFilter
        to_wire_filter( const grab::EventFilter& filter )
        {
            eventgrab::v1::EventFilter wire;
            for( const auto kind : filter.kinds )
            {
                wire.add_kinds( grab::transport::to_wire_kind( kind ) );
            }
            for( const auto category : filter.categories )
            {
                wire.add_categories( grab::transport::to_wire_category( category ) );
            }
            return wire;
        }

        class GrpcSubscriptionStream final : public SubscriptionStream
        {
            public:

                GrpcSubscriptionStream(
                    std::unique_ptr<grpc::ClientContext>                      context,
                    std::unique_ptr<grpc::ClientReader<eventgrab::v1::Event>> reader
                ) :
                    context_( std::move( context ) ),
                    reader_( std::move( reader ) ),
                    reader_thread_(
                        [this]
                        {
                            read_stream();
                        }
                    )
                {
                }

                ~GrpcSubscriptionStream() override
                {
                    context_->TryCancel();
                    if( reader_thread_.joinable() )
                    {
                        reader_thread_.join();
                    }
                }

                GrpcSubscriptionStream( const GrpcSubscriptionStream& ) = delete;
                GrpcSubscriptionStream&
                operator=( const GrpcSubscriptionStream& )         = delete;
                GrpcSubscriptionStream( GrpcSubscriptionStream&& ) = delete;
                GrpcSubscriptionStream&
                operator=( GrpcSubscriptionStream&& ) = delete;

                [[nodiscard]]
                grab::Result<std::optional<grab::SubscriptionEvent>>
                try_next() override
                {
                    const std::scoped_lock lock( mutex_ );
                    if( !events_.empty() )
                    {
                        grab::SubscriptionEvent event = std::move( events_.front() );
                        events_.pop_front();
                        return std::optional<grab::SubscriptionEvent>{
                            std::move( event )
                        };
                    }
                    if( terminal_error_.has_value() )
                    {
                        return std::unexpected( *terminal_error_ );
                    }
                    return std::nullopt;
                }

            private:

                void
                read_stream()
                {
                    eventgrab::v1::Event wire;
                    while( reader_->Read( &wire ) )
                    {
                        auto event = grab::transport::from_wire( wire );
                        if( !event.has_value() )
                        {
                            {
                                const std::scoped_lock lock( mutex_ );
                                terminal_error_ = event.error();
                            }
                            context_->TryCancel();
                            break;
                        }

                        const std::scoped_lock lock( mutex_ );
                        events_.emplace_back( std::move( *event ) );
                    }

                    const auto status = reader_->Finish();
                    if( !status.ok() )
                    {
                        const std::scoped_lock lock( mutex_ );
                        if( !terminal_error_.has_value() )
                        {
                            terminal_error_ = grpc_error( status );
                        }
                    }
                }

                std::unique_ptr<grpc::ClientContext>                      context_;
                std::unique_ptr<grpc::ClientReader<eventgrab::v1::Event>> reader_;
                std::mutex                                                mutex_;
                std::deque<grab::SubscriptionEvent>                       events_;
                std::optional<grab::Error> terminal_error_;
                std::thread                reader_thread_;
        };

    }    // namespace

    class UnixSocketTransport::Impl
    {
        public:

            explicit Impl( const std::string& endpoint ) :
                channel_( grpc::CreateChannel( endpoint,
                                               grpc::InsecureChannelCredentials() ) ),
                stub_( eventgrab::v1::EventGrabService::NewStub( channel_ ) )
            {
            }

            std::shared_ptr<grpc::Channel>                         channel_;
            std::unique_ptr<eventgrab::v1::EventGrabService::Stub> stub_;
    };

    UnixSocketTransport::UnixSocketTransport( std::string endpoint ) :
        endpoint_( std::move( endpoint ) ),
        impl_( std::make_unique<Impl>( endpoint_ ) )
    {
    }

    UnixSocketTransport::~UnixSocketTransport()                                = default;

    UnixSocketTransport::UnixSocketTransport( UnixSocketTransport&& ) noexcept = default;

    UnixSocketTransport&
    UnixSocketTransport::operator=( UnixSocketTransport&& ) noexcept = default;

    grab::Result<void>
    UnixSocketTransport::push_event( grab::Event event )
    {
        auto wire = grab::transport::to_wire( event );
        if( !wire.has_value() )
        {
            return std::unexpected( wire.error() );
        }

        grpc::ClientContext              context;
        eventgrab::v1::PushEventRequest  request;
        eventgrab::v1::PushEventResponse response;
        context.set_deadline( rpc_deadline() );
        *request.mutable_event() = std::move( *wire );

        const auto status = impl_->stub_->PushEvent( &context, request, &response );
        if( !status.ok() )
        {
            return std::unexpected( grpc_error( status ) );
        }
        return {};
    }

    grab::Result<SubscriptionHandle>
    UnixSocketTransport::subscribe( grab::EventFilter filter )
    {
        if( !impl_->channel_->WaitForConnected( rpc_deadline() ) )
        {
            return std::unexpected( grpc_error( grpc::Status{
                grpc::StatusCode::DEADLINE_EXCEEDED,
                "timed out connecting to the daemon",
            } ) );
        }

        auto context = std::make_unique<grpc::ClientContext>();
        auto reader = impl_->stub_->Subscribe( context.get(), to_wire_filter( filter ) );
        // EventService sends initial metadata only after it has validated the
        // filter and registered the EventBus subscription.
        reader->WaitForInitialMetadata();
        SubscriptionHandle stream =
            std::make_unique<GrpcSubscriptionStream>( std::move( context ),
                                                      std::move( reader ) );
        return stream;
    }

    grab::Result<std::vector<grab::EventTypeDescriptor>>
    UnixSocketTransport::list_event_types()
    {
        grpc::ClientContext                   context;
        eventgrab::v1::ListEventTypesRequest  request;
        eventgrab::v1::ListEventTypesResponse response;
        context.set_deadline( rpc_deadline() );
        const auto status = impl_->stub_->ListEventTypes( &context, request, &response );
        if( !status.ok() )
        {
            return std::unexpected( grpc_error( status ) );
        }

        std::vector<grab::EventTypeDescriptor> descriptors;
        descriptors.reserve( static_cast<std::size_t>( response.types_size() ) );
        for( const auto& wire : response.types() )
        {
            const auto kind     = grab::transport::to_grab_kind( wire.kind() );
            const auto category = grab::transport::to_grab_category( wire.category() );
            if( !kind.has_value() || !category.has_value() )
            {
                return grab::fail( grab::ErrorCode::ProtocolError,
                                   "daemon returned an unknown event type" );
            }
            descriptors.push_back( grab::EventTypeDescriptor{
                .kind     = *kind,
                .category = *category,
                .name     = wire.name(),
                .active   = wire.active(),
            } );
        }
        return descriptors;
    }

    const std::string&
    UnixSocketTransport::endpoint() const noexcept
    {
        return endpoint_;
    }

}    // namespace grab::client
