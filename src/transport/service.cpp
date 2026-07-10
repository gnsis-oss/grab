#include "eventgrab/v1/events.pb.h"
#include "eventgrab/v1/service.pb.h"
#include "grab/active_kind_probe.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/event_descriptor.hpp"
#include "grab/result.hpp"
#include "transport/codec.hpp"
#include "transport/proto_descriptor.hpp"
#include "transport/service.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <expected>
#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>
#include <grpcpp/support/status_code_enum.h>
#include <grpcpp/support/sync_stream.h>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace grab::transport
{
    namespace
    {

        constexpr auto subscribePollInterval = std::chrono::milliseconds{ 100 };

        struct NotifyState
        {
                std::mutex              mutex;
                std::condition_variable data_ready;
                bool                    notified = false;
        };

        [[nodiscard]]
        grpc::Status
        invalid_argument( std::string_view message )
        {
            return grpc::Status{
                grpc::StatusCode::INVALID_ARGUMENT,
                std::string{ message },
            };
        }

        [[nodiscard]]
        grpc::Status
        internal_error( std::string_view message )
        {
            return grpc::Status{ grpc::StatusCode::INTERNAL, std::string{ message } };
        }

        [[nodiscard]]
        grab::Result<grab::EventKind>
        from_wire_filter_kind( eventgrab::v1::EventKind kind )
        {
            const auto grab_kind = grab::transport::to_grab_kind( kind );
            if( !grab_kind.has_value() || *grab_kind == grab::EventKind::Unspecified )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "invalid event filter kind" );
            }
            return *grab_kind;
        }

        [[nodiscard]]
        grab::Result<grab::EventCategory>
        from_wire_filter_category( eventgrab::v1::EventCategory category )
        {
            const auto grab_category = grab::transport::to_grab_category( category );
            if( !grab_category.has_value() )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "invalid event filter category" );
            }
            return *grab_category;
        }

        [[nodiscard]]
        grab::Result<grab::EventFilter>
        from_wire_filter( const eventgrab::v1::EventFilter& wire )
        {
            grab::EventFilter filter;
            filter.kinds.reserve( static_cast<std::size_t>( wire.kinds_size() ) );
            filter.categories.reserve(
                static_cast<std::size_t>( wire.categories_size() )
            );

            for( const int kind_value : wire.kinds() )
            {
                auto kind = from_wire_filter_kind(
                    static_cast<eventgrab::v1::EventKind>( kind_value )
                );
                if( !kind.has_value() )
                {
                    return std::unexpected( kind.error() );
                }
                filter.kinds.push_back( *kind );
            }

            for( const int category_value : wire.categories() )
            {
                auto category = from_wire_filter_category(
                    static_cast<eventgrab::v1::EventCategory>( category_value )
                );
                if( !category.has_value() )
                {
                    return std::unexpected( category.error() );
                }
                filter.categories.push_back( *category );
            }

            return filter;
        }

        void
        notify_waiter( const std::shared_ptr<NotifyState>& state )
        {
            {
                const std::scoped_lock lock( state->mutex );
                state->notified = true;
            }
            state->data_ready.notify_one();
        }

        void
        wait_for_data_or_poll_interval( const std::shared_ptr<NotifyState>& state )
        {
            std::unique_lock lock( state->mutex );
            if( !state->notified )
            {
                state->data_ready.wait_for( lock,
                                            subscribePollInterval,
                                            [&]
                                            {
                                                return state->notified;
                                            } );
            }
            state->notified = false;
        }

        [[nodiscard]]
        grpc::Status
        write_available_events( const grpc::ServerContext&                context,
                                grpc::ServerWriter<eventgrab::v1::Event>& writer,
                                grab::Subscription&                       subscription )
        {
            while( true )
            {
                auto event = subscription.try_pop();
                if( !event.has_value() )
                {
                    return grpc::Status::OK;
                }

                auto wire = grab::transport::to_wire( *event );
                if( !wire.has_value() )
                {
                    return internal_error( wire.error().message );
                }

                if( !writer.Write( *wire ) || context.IsCancelled() )
                {
                    return grpc::Status{
                        grpc::StatusCode::CANCELLED,
                        "event stream cancelled"
                    };
                }
            }
        }

    }    // namespace

    EventService::EventService( grab::EventBus&              bus,
                                const grab::ActiveKindProbe* probe ) noexcept :
        bus_( &bus ),
        probe_( probe )
    {
    }

    grpc::Status
    EventService::PushEvent( grpc::ServerContext* /*context*/,
                             const eventgrab::v1::PushEventRequest* request,
                             eventgrab::v1::PushEventResponse* /*response*/ )
    {
        if( request == nullptr )
        {
            return invalid_argument( "missing push request" );
        }

        auto event = grab::transport::from_wire( request->event() );
        if( !event.has_value() )
        {
            return invalid_argument( event.error().message );
        }

        bus_->publish( std::move( *event ) );
        return grpc::Status::OK;
    }

    grpc::Status
    EventService::ListEventTypes(
        grpc::ServerContext* /*context*/,
        const eventgrab::v1::ListEventTypesRequest* /*request*/,
        eventgrab::v1::ListEventTypesResponse* response
    )
    {
        if( response == nullptr )
        {
            return internal_error( "missing list response" );
        }

        for( const auto& row : grab::transport::protoKindRows )
        {
            if( row.kind == grab::EventKind::Unspecified )
            {
                continue;
            }

            auto* type = response->add_types();
            type->set_kind( row.proto_kind );
            type->set_category(
                grab::transport::to_wire_category( grab::category_of( row.kind ) )
            );
            type->set_name( std::string{ grab::wire_name( row.kind ) } );
            type->set_active( probe_ != nullptr && probe_->is_active( row.kind ) );
        }

        return grpc::Status::OK;
    }

    grpc::Status
    EventService::Subscribe( grpc::ServerContext*                      context,
                             const eventgrab::v1::EventFilter*         request,
                             grpc::ServerWriter<eventgrab::v1::Event>* writer )
    {
        if( context == nullptr || request == nullptr || writer == nullptr )
        {
            return invalid_argument( "missing subscribe request" );
        }

        auto filter = from_wire_filter( *request );
        if( !filter.has_value() )
        {
            return invalid_argument( filter.error().message );
        }

        auto subscription = bus_->subscribe( std::move( *filter ) );
        auto notify_state = std::make_shared<NotifyState>();
        subscription.set_notify(
            [notify_state]
            {
                notify_waiter( notify_state );
            }
        );

        // The synchronous gRPC API runs one server thread per subscriber and
        // permits exactly one blocking write here. Async CQ streaming is a
        // future scale optimization, not needed for this correctness path.
        writer->SendInitialMetadata();
        while( !context->IsCancelled() )
        {
            auto status = write_available_events( *context, *writer, subscription );
            if( !status.ok() )
            {
                subscription.set_notify( {} );
                notify_waiter( notify_state );
                return status;
            }

            wait_for_data_or_poll_interval( notify_state );
        }

        subscription.set_notify( {} );
        notify_waiter( notify_state );
        return grpc::Status::OK;
    }

}    // namespace grab::transport
