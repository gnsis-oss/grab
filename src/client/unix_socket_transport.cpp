#include "client/transport.hpp"
#include "client/unix_socket_transport.hpp"
#include "codec/png.hpp"
#include "eventgrab/v1/events.pb.h"
#include "eventgrab/v1/service.grpc.pb.h"
#include "eventgrab/v1/service.pb.h"
#include "grab/capture.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/event_descriptor.hpp"
#include "grab/ids.hpp"
#include "grab/interaction.hpp"
#include "grab/locator.hpp"
#include "grab/query.hpp"
#include "grab/result.hpp"
#include "grab/space.hpp"
#include "grab/trace.hpp"
#include "transport/codec.hpp"
#include "transport/proto_descriptor.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
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
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
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

        constexpr std::string_view grabErrorDetailPrefix{ "grab-error: " };

        [[nodiscard]]
        std::optional<grab::ErrorCode>
        error_code_from_details( std::string_view details ) noexcept
        {
            if( !details.starts_with( grabErrorDetailPrefix ) )
            {
                return std::nullopt;
            }
            const auto name = details.substr( grabErrorDetailPrefix.size() );
            for( const auto& descriptor : grab::error_descriptors() )
            {
                if( descriptor.name == name )
                {
                    return descriptor.code;
                }
            }
            return std::nullopt;
        }

        [[nodiscard]]
        std::uint32_t
        encode_cardinality( grab::Cardinality cardinality ) noexcept
        {
            switch( cardinality )
            {
                case grab::Cardinality::ExactlyOne :
                    return 0U;
                case grab::Cardinality::First :
                    return 1U;
                case grab::Cardinality::All :
                    return 2U;
            }
            return 0U;
        }

        [[nodiscard]]
        std::uint32_t
        encode_routing( grab::RoutePolicy routing ) noexcept
        {
            switch( routing )
            {
                case grab::RoutePolicy::PreferSemantic :
                    return 0U;
                case grab::RoutePolicy::SemanticOnly :
                    return 1U;
                case grab::RoutePolicy::PhysicalOnly :
                    return 2U;
            }
            return 0U;
        }

        [[nodiscard]]
        std::uint32_t
        encode_retry( grab::RetryClass retry ) noexcept
        {
            switch( retry )
            {
                case grab::RetryClass::Never :
                    return 0U;
                case grab::RetryClass::ResolveOnly :
                    return 1U;
                case grab::RetryClass::Idempotent :
                    return 2U;
                case grab::RetryClass::Compensated :
                    return 3U;
            }
            return 0U;
        }

        [[nodiscard]]
        grab::ConsistencyMode
        decode_consistency( std::uint32_t value ) noexcept
        {
            switch( value )
            {
                case 0U :
                    return grab::ConsistencyMode::Live;
                case 1U :
                    return grab::ConsistencyMode::Revisioned;
                case 2U :
                    return grab::ConsistencyMode::Pinned;
                default :
                    return grab::ConsistencyMode::Live;
            }
        }

        void
        encode_widget_ref( const grab::WidgetRef&        ref,
                           eventgrab::v1::WidgetRefWire* wire )
        {
            wire->set_runtime( ref.runtime.value );
            wire->set_tree( ref.tree );
            wire->set_epoch( ref.epoch.value );
            wire->set_node( ref.node );
            wire->set_generation( ref.generation.value );
        }

        [[nodiscard]]
        grab::Match
        decode_match( const eventgrab::v1::MatchWire& wire )
        {
            return grab::Match{
                .ref =
                    grab::WidgetRef{
                                    .runtime    = grab::RuntimeId{ wire.ref().runtime() },
                                    .tree       = wire.ref().tree(),
                                    .epoch      = grab::TreeEpoch{ wire.ref().epoch() },
                                    .node       = wire.ref().node(),
                                    .generation = grab::NodeGeneration{ wire.ref().generation() },
                                    },
                .mode              = decode_consistency( wire.consistency() ),
                .snapshot_revision = wire.snapshot_revision(),
                .matched_predicates =
                    { wire.matched_predicates().begin(),
                                    wire.matched_predicates().end() },
                .provenance = grab::ProviderProvenance{
                                    .provider           = wire.provider(),
                                    .candidate_provider = wire.candidate_provider(),
                                    .runtime            = grab::RuntimeId{ wire.provenance_runtime() },
                                    .revision           = wire.provenance_revision(),
                                    },
            };
        }

        [[nodiscard]]
        grab::Error
        grpc_error( const grpc::Status& status )
        {
            if( const auto typed = error_code_from_details( status.error_details() );
                typed.has_value() )
            {
                auto message = status.error_message();
                if( message.empty() )
                {
                    message = "gRPC transport request failed";
                }
                return grab::Error{
                    .code        = *typed,
                    .message     = std::move( message ),
                    .capability  = {},
                    .target      = {},
                    .attempts    = {},
                    .disposition = grab::default_disposition_of( *typed ),
                    .diagnostics = {},
                };
            }

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

    grab::Result<grab::Match>
    UnixSocketTransport::resolve( const grab::Locator& locator,
                                  grab::Cardinality    cardinality )
    {
        grpc::ClientContext                context;
        eventgrab::v1::ResolveNodeRequest  request;
        eventgrab::v1::ResolveNodeResponse response;
        context.set_deadline( rpc_deadline() );
        request.set_locator( locator.to_string() );
        request.set_cardinality( encode_cardinality( cardinality ) );

        const auto status = impl_->stub_->ResolveNode( &context, request, &response );
        if( !status.ok() )
        {
            return std::unexpected( grpc_error( status ) );
        }
        return decode_match( response.match() );
    }

    grab::Result<grab::Receipt>
    UnixSocketTransport::perform( const grab::Action&        action,
                                  const grab::ActionOptions& options )
    {
        grpc::ClientContext                  context;
        eventgrab::v1::PerformActionRequest  request;
        eventgrab::v1::PerformActionResponse response;
        context.set_deadline( rpc_deadline() );

        if( std::holds_alternative<grab::Drag>( action ) ||
            std::holds_alternative<grab::PressKey>( action ) )
        {
            return grab::fail(
                grab::ErrorCode::InvalidArgument,
                "drag and press-key actions are not yet expressible over the socket "
                "wire; use the in-process (loopback) transport"
            );
        }

        const auto encode_target = [&request]( const grab::ActionTarget& target )
        {
            std::visit(
                [&request]( const auto& value )
                {
                    using Target = std::decay_t<decltype( value )>;
                    if constexpr( std::is_same_v<Target, grab::Locator> )
                    {
                        request.set_locator( value.to_string() );
                    }
                    else
                    {
                        encode_widget_ref( value.ref, request.mutable_target_ref() );
                    }
                },
                target
            );
        };

        std::visit(
            [&request, &encode_target]( const auto& value )
            {
                using ActionType = std::decay_t<decltype( value )>;
                if constexpr( std::is_same_v<ActionType, grab::Click> )
                {
                    request.set_command( "input.click" );
                    encode_target( value.target );
                }
                else if constexpr( std::is_same_v<ActionType, grab::TypeText> )
                {
                    request.set_command( "input.type" );
                    request.set_text( value.text );
                    encode_target( value.target );
                }
                // Drag / PressKey are rejected before this visit; nothing to encode.
            },
            action
        );

        auto* wire_options = request.mutable_options();
        wire_options->set_deadline_ms( static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>( options.deadline )
                .count()
        ) );
        wire_options->set_cardinality( encode_cardinality( options.cardinality ) );
        wire_options->set_routing( encode_routing( options.routing ) );
        wire_options->set_retry( encode_retry( options.retry ) );
        wire_options->set_force( options.force );

        const auto status = impl_->stub_->PerformAction( &context, request, &response );
        if( !status.ok() )
        {
            return std::unexpected( grpc_error( status ) );
        }

        grab::Receipt receipt{};
        const auto&   wire = response.receipt();
        if( const auto commit =
                grab::detail::commit_status_name.value_of( wire.commit_status() );
            commit.has_value() )
        {
            receipt.commit = *commit;
        }
        receipt.fallback_used     = wire.fallback_used();
        receipt.forced            = wire.forced();
        receipt.locator           = wire.locator();
        receipt.snapshot_revision = wire.snapshot_revision();
        receipt.routes.reserve( static_cast<std::size_t>( wire.routes_size() ) );
        for( const auto& route : wire.routes() )
        {
            receipt.routes.push_back( grab::RouteAttempt{
                .route     = route,
                .selected  = false,
                .rejection = {},
                .detail    = {},
            } );
        }
        return receipt;
    }

    grab::Result<grab::Frame>
    UnixSocketTransport::capture( const grab::CaptureTarget&  target,
                                  const grab::CaptureOptions& options )
    {
        grpc::ClientContext                 context;
        eventgrab::v1::CaptureFrameRequest  request;
        eventgrab::v1::CaptureFrameResponse response;
        context.set_deadline( rpc_deadline() );

        if( const auto* output = std::get_if<std::string>( &target ) )
        {
            request.set_output( *output );
        }
        else
        {
            return grab::fail(
                grab::ErrorCode::InvalidArgument,
                "socket capture requires an output name; node-grade capture targets "
                "are not yet expressible over the wire"
            );
        }

        // Capture options are currently enforced by the server-side defaults.
        static_cast<void>( options );
        const auto status = impl_->stub_->CaptureFrame( &context, request, &response );
        if( !status.ok() )
        {
            return std::unexpected( grpc_error( status ) );
        }

        const auto& png   = response.png();
        auto        image = grab::codec::decode_png(
            std::as_bytes( std::span{ png.data(), png.size() } )
        );
        if( !image.has_value() )
        {
            return std::unexpected( std::move( image.error() ) );
        }
        const auto& meta = response.meta();
        return grab::Frame{
            .id    = grab::FrameId{ meta.frame_id() },
            .image = std::move( *image ),
            .space =
                grab::CoordinateSpaceId{
                    static_cast<decltype( grab::CoordinateSpaceId{}.value )>(
                        meta.space()
                    )
                },
            .generation     = grab::CaptureGeneration{ static_cast<std::uint32_t>(
                meta.generation()
            ) },
            .captured_at_ns = meta.captured_at_ns(),
            .content_rect   = {},
            .scale          = 1.0,
        };
    }

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
