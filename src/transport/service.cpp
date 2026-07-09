#include "eventgrab/v1/events.pb.h"
#include "eventgrab/v1/service.pb.h"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/result.hpp"
#include "transport/codec.hpp"
#include "transport/service.hpp"

#include <array>
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

        constexpr auto kSubscribePollInterval = std::chrono::milliseconds{ 100 };

        struct EventTypeRecord
        {
                eventgrab::v1::EventKind     kind;
                eventgrab::v1::EventCategory category;
        };

        struct NotifyState
        {
                std::mutex              mutex;
                std::condition_variable data_ready;
                bool                    notified = false;
        };

        constexpr auto kKnownEventTypes = std::to_array<EventTypeRecord>( {
            EventTypeRecord{
                            .kind     = eventgrab::v1::INPUT_KEY_DOWN,
                            .category = eventgrab::v1::EVENT_CATEGORY_INPUT,
                            },
            EventTypeRecord{
                            .kind     = eventgrab::v1::INPUT_KEY_UP,
                            .category = eventgrab::v1::EVENT_CATEGORY_INPUT,
                            },
            EventTypeRecord{
                            .kind     = eventgrab::v1::INPUT_KEY_COMBO,
                            .category = eventgrab::v1::EVENT_CATEGORY_INPUT,
                            },
            EventTypeRecord{
                            .kind     = eventgrab::v1::INPUT_MOUSE_CLICK,
                            .category = eventgrab::v1::EVENT_CATEGORY_INPUT,
                            },
            EventTypeRecord{
                            .kind     = eventgrab::v1::INPUT_MOUSE_MOVE,
                            .category = eventgrab::v1::EVENT_CATEGORY_INPUT,
                            },
            EventTypeRecord{
                            .kind     = eventgrab::v1::INPUT_IDLE_START,
                            .category = eventgrab::v1::EVENT_CATEGORY_INPUT,
                            },
            EventTypeRecord{
                            .kind     = eventgrab::v1::INPUT_IDLE_END,
                            .category = eventgrab::v1::EVENT_CATEGORY_INPUT,
                            },
            EventTypeRecord{
                            .kind     = eventgrab::v1::WINDOW_FOCUS_CHANGED,
                            .category = eventgrab::v1::EVENT_CATEGORY_WINDOW,
                            },
            EventTypeRecord{
                            .kind     = eventgrab::v1::WINDOW_TITLE_CHANGED,
                            .category = eventgrab::v1::EVENT_CATEGORY_WINDOW,
                            },
            EventTypeRecord{
                            .kind     = eventgrab::v1::WINDOW_CREATED,
                            .category = eventgrab::v1::EVENT_CATEGORY_WINDOW,
                            },
            EventTypeRecord{
                            .kind     = eventgrab::v1::WINDOW_CLOSED,
                            .category = eventgrab::v1::EVENT_CATEGORY_WINDOW,
                            },
            EventTypeRecord{
                            .kind     = eventgrab::v1::A11Y_BUTTON_CLICKED,
                            .category = eventgrab::v1::EVENT_CATEGORY_ACCESSIBILITY,
                            },
            EventTypeRecord{
                            .kind     = eventgrab::v1::A11Y_MENU_OPENED,
                            .category = eventgrab::v1::EVENT_CATEGORY_ACCESSIBILITY,
                            },
            EventTypeRecord{
                            .kind     = eventgrab::v1::A11Y_MENU_CLOSED,
                            .category = eventgrab::v1::EVENT_CATEGORY_ACCESSIBILITY,
                            },
            EventTypeRecord{
                            .kind     = eventgrab::v1::A11Y_FOCUS_CHANGED,
                            .category = eventgrab::v1::EVENT_CATEGORY_ACCESSIBILITY,
                            },
            EventTypeRecord{
                            .kind     = eventgrab::v1::A11Y_TEXT_CHANGED,
                            .category = eventgrab::v1::EVENT_CATEGORY_ACCESSIBILITY,
                            },
            EventTypeRecord{
                            .kind     = eventgrab::v1::A11Y_STATE_CHANGED,
                            .category = eventgrab::v1::EVENT_CATEGORY_ACCESSIBILITY,
                            },
            EventTypeRecord{
                            .kind     = eventgrab::v1::APP_TAB_CHANGED,
                            .category = eventgrab::v1::EVENT_CATEGORY_INTEGRATION,
                            },
            EventTypeRecord{
                            .kind     = eventgrab::v1::APP_CONTEXT_UPDATE,
                            .category = eventgrab::v1::EVENT_CATEGORY_INTEGRATION,
                            },
            EventTypeRecord{
                            .kind     = eventgrab::v1::BROWSER_TAB_SWITCHED,
                            .category = eventgrab::v1::EVENT_CATEGORY_BROWSER,
                            },
            EventTypeRecord{
                            .kind     = eventgrab::v1::STATE_SNAPSHOT,
                            .category = eventgrab::v1::EVENT_CATEGORY_STATE,
                            },
        } );

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
            switch( kind )
            {
                case eventgrab::v1::INPUT_KEY_DOWN :
                    return grab::EventKind::key_down;
                case eventgrab::v1::INPUT_KEY_UP :
                    return grab::EventKind::key_up;
                case eventgrab::v1::INPUT_KEY_COMBO :
                    return grab::EventKind::key_combo;
                case eventgrab::v1::INPUT_MOUSE_CLICK :
                    return grab::EventKind::mouse_click;
                case eventgrab::v1::INPUT_MOUSE_MOVE :
                    return grab::EventKind::mouse_move;
                case eventgrab::v1::INPUT_IDLE_START :
                    return grab::EventKind::idle_start;
                case eventgrab::v1::INPUT_IDLE_END :
                    return grab::EventKind::idle_end;
                case eventgrab::v1::WINDOW_FOCUS_CHANGED :
                    return grab::EventKind::window_focus_changed;
                case eventgrab::v1::WINDOW_TITLE_CHANGED :
                    return grab::EventKind::window_title_changed;
                case eventgrab::v1::WINDOW_CREATED :
                    return grab::EventKind::window_created;
                case eventgrab::v1::WINDOW_CLOSED :
                    return grab::EventKind::window_closed;
                case eventgrab::v1::A11Y_BUTTON_CLICKED :
                    return grab::EventKind::a11y_button_clicked;
                case eventgrab::v1::A11Y_MENU_OPENED :
                    return grab::EventKind::a11y_menu_opened;
                case eventgrab::v1::A11Y_MENU_CLOSED :
                    return grab::EventKind::a11y_menu_closed;
                case eventgrab::v1::A11Y_FOCUS_CHANGED :
                    return grab::EventKind::a11y_focus_changed;
                case eventgrab::v1::A11Y_TEXT_CHANGED :
                    return grab::EventKind::a11y_text_changed;
                case eventgrab::v1::A11Y_STATE_CHANGED :
                    return grab::EventKind::a11y_state_changed;
                case eventgrab::v1::APP_TAB_CHANGED :
                    return grab::EventKind::app_tab_changed;
                case eventgrab::v1::APP_CONTEXT_UPDATE :
                    return grab::EventKind::app_context_update;
                case eventgrab::v1::BROWSER_TAB_SWITCHED :
                    return grab::EventKind::browser_tab_switched;
                case eventgrab::v1::STATE_SNAPSHOT :
                    return grab::EventKind::state_snapshot;
                case eventgrab::v1::EVENT_KIND_UNSPECIFIED :
                default :
                    return grab::fail( grab::ErrorCode::invalid_argument,
                                       "invalid event filter kind" );
            }
        }

        [[nodiscard]]
        grab::Result<grab::EventCategory>
        from_wire_filter_category( eventgrab::v1::EventCategory category )
        {
            switch( category )
            {
                case eventgrab::v1::EVENT_CATEGORY_INPUT :
                    return grab::EventCategory::input;
                case eventgrab::v1::EVENT_CATEGORY_WINDOW :
                    return grab::EventCategory::window;
                case eventgrab::v1::EVENT_CATEGORY_ACCESSIBILITY :
                    return grab::EventCategory::accessibility;
                case eventgrab::v1::EVENT_CATEGORY_INTEGRATION :
                    return grab::EventCategory::integration;
                case eventgrab::v1::EVENT_CATEGORY_BROWSER :
                    return grab::EventCategory::browser;
                case eventgrab::v1::EVENT_CATEGORY_STATE :
                    return grab::EventCategory::state;
                case eventgrab::v1::EVENT_CATEGORY_UNSPECIFIED :
                default :
                    return grab::fail( grab::ErrorCode::invalid_argument,
                                       "invalid event filter category" );
            }
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
                                            kSubscribePollInterval,
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

    EventService::EventService( grab::EventBus& bus ) noexcept :
        bus_( &bus )
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

        // Producer tracking lands with real backends; until then the service
        // reports the full static kind registry with active=false.
        for( const auto& record : kKnownEventTypes )
        {
            auto* type = response->add_types();
            type->set_kind( record.kind );
            type->set_category( record.category );
            type->set_name( eventgrab::v1::EventKind_Name( record.kind ) );
            type->set_active( false );
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
