#include "eventgrab/v1/events.pb.h"
#include "eventgrab/v1/service.grpc.pb.h"
#include "eventgrab/v1/service.pb.h"
#include "grab/active_kind_probe.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/event_descriptor.hpp"
#include "transport/codec.hpp"
#include "transport/proto_descriptor.hpp"
#include "transport/server.hpp"
#include "transport/service.hpp"

#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/support/channel_arguments.h>
#include <grpcpp/support/status.h>
#include <grpcpp/support/status_code_enum.h>

// clang-format off
#include <gtest/gtest.h>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
// clang-format on

namespace
{

    constexpr std::string_view unixEndpointPrefix  = "unix:";
    constexpr std::string_view socketStem          = "grab-event-service-";
    constexpr std::string_view nameSeparator       = "-";
    constexpr std::string_view unknownTestName     = "unknown";
    constexpr std::string_view socketExtension     = ".sock";
    constexpr auto             unaryDeadline       = std::chrono::seconds{ 2 };
    constexpr auto             streamDeadline      = std::chrono::seconds{ 5 };
    constexpr auto             streamReadyTimeout  = std::chrono::seconds{ 2 };
    constexpr auto             streamResultTimeout = std::chrono::seconds{ 2 };
    constexpr auto             noEventWindow       = std::chrono::milliseconds{ 200 };
    constexpr double           eventTimestamp      = 1729.25;
    constexpr std::uint64_t    noSequence          = 0U;
    constexpr std::uint32_t    keyDownCode         = 30U;
    constexpr std::uint32_t    keyUpCode           = 31U;
    constexpr std::string_view keyDownName         = "A";
    constexpr std::string_view keyUpName           = "B";
    constexpr auto             invalidArgumentCode = grpc::StatusCode::INVALID_ARGUMENT;
    constexpr auto             cancelledCode       = grpc::StatusCode::CANCELLED;

    class FixedProbe final : public grab::ActiveKindProbe
    {
        public:

            explicit FixedProbe( std::set<grab::EventKind> active ) :
                active_( std::move( active ) )
            {
            }

            ~FixedProbe() override          = default;

            FixedProbe( const FixedProbe& ) = delete;
            FixedProbe&
            operator=( const FixedProbe& ) = delete;
            FixedProbe( FixedProbe&& )     = delete;
            FixedProbe&
            operator=( FixedProbe&& ) = delete;

            [[nodiscard]]
            bool
            is_active( grab::EventKind kind ) const noexcept override
            {
                return active_.contains( kind );
            }

        private:

            std::set<grab::EventKind> active_;
    };

    [[nodiscard]]
    std::string
    current_test_socket_name()
    {
        const auto* const test_info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        if( test_info == nullptr )
        {
            return std::string{ socketStem } +
                   std::string{ unknownTestName } +
                   std::string{ socketExtension };
        }

        return std::string{ socketStem } +
               test_info->test_suite_name() +
               std::string{ nameSeparator } +
               test_info->name() +
               std::string{ socketExtension };
    }

    [[nodiscard]]
    std::filesystem::path
    socket_path()
    {
        return std::filesystem::temp_directory_path() / current_test_socket_name();
    }

    void
    remove_if_present( const std::filesystem::path& path ) noexcept
    {
        std::error_code ignored;
        const bool      removed = std::filesystem::remove( path, ignored );
        static_cast<void>( removed );
        static_cast<void>( ignored.value() );
    }

    class UnixEndpoint
    {
        public:

            UnixEndpoint() :
                path_( socket_path() ),
                endpoint_( std::string{ unixEndpointPrefix } + path_.string() )
            {
                remove_if_present( path_ );
            }

            ~UnixEndpoint() noexcept
            {
                remove_if_present( path_ );
            }

            UnixEndpoint( const UnixEndpoint& ) = delete;
            UnixEndpoint&
            operator=( const UnixEndpoint& ) = delete;
            UnixEndpoint( UnixEndpoint&& )   = delete;
            UnixEndpoint&
            operator=( UnixEndpoint&& ) = delete;

            [[nodiscard]]
            const std::string&
            value() const noexcept
            {
                return endpoint_;
            }

        private:

            std::filesystem::path path_;
            std::string           endpoint_;
    };

    struct StreamReadResult
    {
            bool                 got_event = false;
            eventgrab::v1::Event event;
            grpc::Status         status;
    };

    struct RunningSubscribe
    {
            std::unique_ptr<grpc::ClientContext> context = nullptr;
            std::future<void>                    ready;
            std::future<StreamReadResult>        result;
            std::thread                          thread;

            RunningSubscribe() = default;

            ~RunningSubscribe() noexcept
            {
                stop();
            }

            RunningSubscribe( const RunningSubscribe& ) = delete;
            RunningSubscribe&
            operator=( const RunningSubscribe& )                  = delete;
            RunningSubscribe( RunningSubscribe&& other ) noexcept = default;

            RunningSubscribe&
            operator=( RunningSubscribe&& other ) noexcept
            {
                if( this != &other )
                {
                    stop();
                    context = std::move( other.context );
                    ready   = std::move( other.ready );
                    result  = std::move( other.result );
                    thread  = std::move( other.thread );
                }
                return *this;
            }

            void
            cancel() const noexcept
            {
                if( context != nullptr )
                {
                    context->TryCancel();
                }
            }

            void
            join() noexcept
            {
                if( thread.joinable() )
                {
                    thread.join();
                }
            }

            void
            stop() noexcept
            {
                cancel();
                join();
            }
    };

    [[nodiscard]]
    grab::Event
    make_key_event( grab::EventKind  kind,
                    std::uint32_t    code,
                    std::string_view name )
    {
        return grab::Event{
            .timestamp = eventTimestamp,
            .sequence  = noSequence,
            .kind      = kind,
            .category  = grab::category_of( kind ),
            .payload   = grab::Payload{ grab::InputKey{
                .code = code,
                .name = std::string{ name },
            } },
        };
    }

    [[nodiscard]]
    eventgrab::v1::Event
    make_wire_event( const grab::Event& event )
    {
        auto wire = grab::transport::to_wire( event );
        EXPECT_TRUE( wire.has_value() );
        if( !wire.has_value() )
        {
            return {};
        }
        return *wire;
    }

    [[nodiscard]]
    eventgrab::v1::Event
    key_down_wire()
    {
        return make_wire_event(
            make_key_event( grab::EventKind::KeyDown, keyDownCode, keyDownName )
        );
    }

    [[nodiscard]]
    eventgrab::v1::Event
    key_up_wire()
    {
        return make_wire_event(
            make_key_event( grab::EventKind::KeyUp, keyUpCode, keyUpName )
        );
    }

    [[nodiscard]]
    std::unique_ptr<eventgrab::v1::EventGrabService::Stub>
    make_stub( const std::string& endpoint )
    {
        auto channel =
            grpc::CreateChannel( endpoint, grpc::InsecureChannelCredentials() );
        return eventgrab::v1::EventGrabService::NewStub( channel );
    }

    class TestServer
    {
        public:

            explicit TestServer( grab::EventBus&              bus,
                                 const grab::ActiveKindProbe* probe = nullptr )
            {
                auto transport =
                    grab::transport::TransportServer::start( endpoint_.value(),
                                                             bus,
                                                             probe );
                if( transport.has_value() )
                {
                    transport_.emplace( std::move( *transport ) );
                    stub_ = make_stub( endpoint_.value() );
                    return;
                }

                service_ = std::make_unique<grab::transport::EventService>( bus, probe );
                grpc::ServerBuilder builder;
                builder.RegisterService( service_.get() );
                in_process_ = builder.BuildAndStart();
                if( in_process_ != nullptr )
                {
                    const grpc::ChannelArguments arguments;
                    stub_ = eventgrab::v1::EventGrabService::NewStub(
                        in_process_->InProcessChannel( arguments )
                    );
                }
            }

            ~TestServer() noexcept
            {
                shutdown();
            }

            TestServer( const TestServer& ) = delete;
            TestServer&
            operator=( const TestServer& ) = delete;
            TestServer( TestServer&& )     = delete;
            TestServer&
            operator=( TestServer&& ) = delete;

            [[nodiscard]]
            bool
            started() const noexcept
            {
                return stub_ != nullptr;
            }

            [[nodiscard]]
            eventgrab::v1::EventGrabService::Stub&
            stub() const noexcept
            {
                return *stub_;
            }

            void
            shutdown() noexcept
            {
                if( transport_.has_value() )
                {
                    transport_->shutdown();
                    transport_.reset();
                }

                if( in_process_ != nullptr )
                {
                    in_process_->Shutdown();
                    in_process_->Wait();
                    in_process_.reset();
                    service_.reset();
                }
            }

        private:

            UnixEndpoint                                           endpoint_;
            std::optional<grab::transport::TransportServer>        transport_;
            std::unique_ptr<grab::transport::EventService>         service_    = nullptr;
            std::unique_ptr<grpc::Server>                          in_process_ = nullptr;
            std::unique_ptr<eventgrab::v1::EventGrabService::Stub> stub_       = nullptr;
    };

    [[nodiscard]]
    grpc::Status
    push_wire_event( eventgrab::v1::EventGrabService::Stub& stub,
                     const eventgrab::v1::Event&            wire )
    {
        grpc::ClientContext context;
        context.set_deadline( std::chrono::system_clock::now() + unaryDeadline );

        eventgrab::v1::PushEventRequest  request;
        eventgrab::v1::PushEventResponse response;
        *request.mutable_event() = wire;
        return stub.PushEvent( &context, request, &response );
    }

    [[nodiscard]]
    RunningSubscribe
    start_subscribe( eventgrab::v1::EventGrabService::Stub& stub,
                     eventgrab::v1::EventFilter             filter )
    {
        RunningSubscribe running;
        running.context = std::make_unique<grpc::ClientContext>();
        running.context->set_deadline( std::chrono::system_clock::now() +
                                       streamDeadline );

        std::promise<void>             ready_promise;
        std::promise<StreamReadResult> result_promise;
        running.ready  = ready_promise.get_future();
        running.result = result_promise.get_future();

        auto* context  = running.context.get();
        running.thread = std::thread(
            [&stub,
             context,
             filter         = std::move( filter ),
             ready_promise  = std::move( ready_promise ),
             result_promise = std::move( result_promise )]() mutable
            {
                auto reader = stub.Subscribe( context, filter );
                reader->WaitForInitialMetadata();
                ready_promise.set_value();

                eventgrab::v1::Event event;
                const bool           got_event = reader->Read( &event );
                if( got_event )
                {
                    context->TryCancel();
                }

                auto status = reader->Finish();
                result_promise.set_value( StreamReadResult{
                    .got_event = got_event,
                    .event     = std::move( event ),
                    .status    = std::move( status ),
                } );
            }
        );

        return running;
    }

    [[nodiscard]]
    StreamReadResult
    finish_subscription( RunningSubscribe& subscriber )
    {
        if( subscriber.result.wait_for( streamResultTimeout ) !=
            std::future_status::ready )
        {
            subscriber.cancel();
        }

        subscriber.join();
        return subscriber.result.get();
    }

    void
    expect_wire_event_eq( const eventgrab::v1::Event& expected,
                          const eventgrab::v1::Event& actual )
    {
        EXPECT_EQ( actual.kind(), expected.kind() );
        EXPECT_EQ( actual.category(), expected.category() );
        EXPECT_DOUBLE_EQ( actual.timestamp(), expected.timestamp() );
        ASSERT_EQ( actual.data_size(), expected.data_size() );
        for( const auto& [key, value] : expected.data() )
        {
            const auto found = actual.data().find( key );
            ASSERT_NE( found, actual.data().end() );
            EXPECT_EQ( found->second, value );
        }
    }

    [[nodiscard]]
    const eventgrab::v1::EventTypeInfo*
    find_type( const eventgrab::v1::ListEventTypesResponse& response,
               eventgrab::v1::EventKind                     kind )
    {
        for( const auto& type : response.types() )
        {
            if( type.kind() == kind )
            {
                return &type;
            }
        }
        return nullptr;
    }

    void
    list_event_types_or_fail( eventgrab::v1::EventGrabService::Stub& stub,
                              eventgrab::v1::ListEventTypesResponse& response )
    {
        grpc::ClientContext context;
        context.set_deadline( std::chrono::system_clock::now() + unaryDeadline );

        const eventgrab::v1::ListEventTypesRequest request;
        const auto status = stub.ListEventTypes( &context, request, &response );

        ASSERT_TRUE( status.ok() ) << status.error_message();
    }

    void
    expect_type_info( const eventgrab::v1::ListEventTypesResponse& response,
                      eventgrab::v1::EventKind                     kind,
                      eventgrab::v1::EventCategory                 category )
    {
        const auto* type = find_type( response, kind );
        ASSERT_NE( type, nullptr );
        EXPECT_EQ( type->category(), category );
        const auto grab_kind = grab::transport::to_grab_kind( kind );
        ASSERT_TRUE( grab_kind.has_value() );
        EXPECT_EQ( type->name(), grab::wire_name( *grab_kind ) );
        EXPECT_FALSE( type->active() );
    }

    [[nodiscard]]
    bool
    stream_finished_cleanly( const grpc::Status& status ) noexcept
    {
        return status.ok() || status.error_code() == cancelledCode;
    }

}    // namespace

TEST( ServiceOptions,
      DefaultsAreVisibleAndOverridable )
{
    constexpr auto customPollInterval = std::chrono::milliseconds{ 20 };

    constexpr grab::transport::ServiceOptions defaults;
    EXPECT_EQ( defaults.poll_interval,
               grab::transport::ServiceOptions::defaultPollInterval );
    EXPECT_EQ( defaults.poll_interval, std::chrono::milliseconds{ 100 } );

    constexpr grab::transport::ServiceOptions customized{
        .poll_interval = customPollInterval,
    };
    EXPECT_EQ( customized.poll_interval, customPollInterval );
}

TEST( EventService,
      PushedEventReachesSubscriber )
{
    grab::EventBus   bus;
    const TestServer server{ bus };
    ASSERT_TRUE( server.started() );

    const eventgrab::v1::EventFilter filter;
    auto subscriber = start_subscribe( server.stub(), filter );
    ASSERT_EQ( subscriber.ready.wait_for( streamReadyTimeout ),
               std::future_status::ready );

    const auto expected = key_down_wire();
    const auto status   = push_wire_event( server.stub(), expected );
    ASSERT_TRUE( status.ok() ) << status.error_message();

    const auto result = finish_subscription( subscriber );
    ASSERT_TRUE( result.got_event );
    expect_wire_event_eq( expected, result.event );
    EXPECT_TRUE( stream_finished_cleanly( result.status ) );
}

TEST( EventService,
      MalformedPushRejectedAndNotDelivered )
{
    grab::EventBus   bus;
    const TestServer server{ bus };
    ASSERT_TRUE( server.started() );

    const eventgrab::v1::EventFilter filter;
    auto subscriber = start_subscribe( server.stub(), filter );
    ASSERT_EQ( subscriber.ready.wait_for( streamReadyTimeout ),
               std::future_status::ready );

    eventgrab::v1::Event malformed;
    malformed.set_kind( eventgrab::v1::EVENT_KIND_UNSPECIFIED );
    malformed.set_category( eventgrab::v1::EVENT_CATEGORY_INPUT );
    malformed.set_timestamp( eventTimestamp );

    const auto status = push_wire_event( server.stub(), malformed );
    EXPECT_EQ( status.error_code(), invalidArgumentCode );
    EXPECT_EQ( subscriber.result.wait_for( noEventWindow ),
               std::future_status::timeout );

    subscriber.cancel();
    const auto result = finish_subscription( subscriber );
    EXPECT_FALSE( result.got_event );
    EXPECT_TRUE( stream_finished_cleanly( result.status ) );
}

TEST( EventService,
      FilterDeliversOnlyMatchingKinds )
{
    grab::EventBus   bus;
    const TestServer server{ bus };
    ASSERT_TRUE( server.started() );

    eventgrab::v1::EventFilter filter;
    filter.add_kinds( eventgrab::v1::INPUT_KEY_DOWN );
    auto subscriber = start_subscribe( server.stub(), filter );
    ASSERT_EQ( subscriber.ready.wait_for( streamReadyTimeout ),
               std::future_status::ready );

    const auto ignored_status = push_wire_event( server.stub(), key_up_wire() );
    ASSERT_TRUE( ignored_status.ok() ) << ignored_status.error_message();
    EXPECT_EQ( subscriber.result.wait_for( noEventWindow ),
               std::future_status::timeout );

    const auto expected         = key_down_wire();
    const auto delivered_status = push_wire_event( server.stub(), expected );
    ASSERT_TRUE( delivered_status.ok() ) << delivered_status.error_message();

    const auto result = finish_subscription( subscriber );
    ASSERT_TRUE( result.got_event );
    expect_wire_event_eq( expected, result.event );
    EXPECT_TRUE( stream_finished_cleanly( result.status ) );
}

TEST( EventService,
      ListEventTypesReturnsKinds )
{
    grab::EventBus   bus;
    const TestServer server{ bus };
    ASSERT_TRUE( server.started() );

    eventgrab::v1::ListEventTypesResponse response;
    list_event_types_or_fail( server.stub(), response );

    expect_type_info( response,
                      eventgrab::v1::INPUT_KEY_DOWN,
                      eventgrab::v1::EVENT_CATEGORY_INPUT );
    expect_type_info( response,
                      eventgrab::v1::INPUT_MOUSE_MOVE,
                      eventgrab::v1::EVENT_CATEGORY_INPUT );
    expect_type_info( response,
                      eventgrab::v1::BROWSER_TAB_SWITCHED,
                      eventgrab::v1::EVENT_CATEGORY_BROWSER );
    expect_type_info( response,
                      eventgrab::v1::STATE_SNAPSHOT,
                      eventgrab::v1::EVENT_CATEGORY_STATE );
}

TEST( EventService,
      ListEventTypesReturnsDottedNames )
{
    grab::EventBus   bus;
    const TestServer server{ bus };
    ASSERT_TRUE( server.started() );

    eventgrab::v1::ListEventTypesResponse response;
    list_event_types_or_fail( server.stub(), response );

    for( const auto& type : response.types() )
    {
        const auto kind = grab::transport::to_grab_kind( type.kind() );
        ASSERT_TRUE( kind.has_value() );
        ASSERT_NE( *kind, grab::EventKind::Unspecified );
        EXPECT_EQ( type.name(), grab::wire_name( *kind ) );
        EXPECT_NE( type.name().find( '.' ), std::string::npos );
    }
}

TEST( EventService,
      ListEventTypesUsesActiveKindProbe )
{
    grab::EventBus   bus;
    const FixedProbe probe{
        {
         grab::EventKind::KeyDown,
         grab::EventKind::WindowFocusChanged,
         }
    };
    const TestServer server{ bus, &probe };
    ASSERT_TRUE( server.started() );

    eventgrab::v1::ListEventTypesResponse response;
    list_event_types_or_fail( server.stub(), response );

    for( const auto& type : response.types() )
    {
        const auto kind = grab::transport::to_grab_kind( type.kind() );
        ASSERT_TRUE( kind.has_value() );
        EXPECT_EQ( type.active(), probe.is_active( *kind ) ) << type.name();
    }
}

TEST( EventService,
      ListEventTypeNamesRoundTripThroughWireKind )
{
    grab::EventBus   bus;
    const TestServer server{ bus };
    ASSERT_TRUE( server.started() );

    eventgrab::v1::ListEventTypesResponse response;
    list_event_types_or_fail( server.stub(), response );

    for( const auto& type : response.types() )
    {
        const auto kind = grab::wire_kind( type.name() );
        ASSERT_TRUE( kind.has_value() );
        EXPECT_EQ( grab::transport::to_wire_kind( *kind ), type.kind() );
    }
}

TEST( EventService,
      ListEventTypesCoversEveryKindOnce )
{
    grab::EventBus   bus;
    const TestServer server{ bus };
    ASSERT_TRUE( server.started() );

    eventgrab::v1::ListEventTypesResponse response;
    list_event_types_or_fail( server.stub(), response );

    EXPECT_EQ( response.types_size(),
               static_cast<int>( grab::detail::eventDescriptors.size() - 1U ) );

    std::set<grab::EventKind> seen;
    for( const auto& type : response.types() )
    {
        EXPECT_NE( type.kind(), eventgrab::v1::EVENT_KIND_UNSPECIFIED );
        const auto kind = grab::transport::to_grab_kind( type.kind() );
        ASSERT_TRUE( kind.has_value() );
        EXPECT_NE( *kind, grab::EventKind::Unspecified );
        EXPECT_TRUE( seen.insert( *kind ).second );
    }

    for( const auto& descriptor : grab::detail::eventDescriptors )
    {
        if( descriptor.kind == grab::EventKind::Unspecified )
        {
            continue;
        }
        EXPECT_TRUE( seen.contains( descriptor.kind ) );
    }
}

TEST( EventService,
      ClientCancelStopsStreamCleanly )
{
    grab::EventBus bus;
    TestServer     server{ bus };
    ASSERT_TRUE( server.started() );

    const eventgrab::v1::EventFilter filter;
    auto subscriber = start_subscribe( server.stub(), filter );
    ASSERT_EQ( subscriber.ready.wait_for( streamReadyTimeout ),
               std::future_status::ready );

    subscriber.cancel();
    const auto result = finish_subscription( subscriber );
    EXPECT_FALSE( result.got_event );
    EXPECT_TRUE( stream_finished_cleanly( result.status ) );

    server.shutdown();
}

TEST( EventService,
      ClientContextIsReturnedWithMonotonicDiagnostics )
{
    constexpr std::uint64_t firstClientSequence  = 41U;
    constexpr std::uint64_t secondClientSequence = firstClientSequence + 1U;

    grab::EventBus          bus;
    const TestServer        server{ bus };
    ASSERT_TRUE( server.started() );

    eventgrab::v1::SetClientContextRequest first_request;
    first_request.set_context( "client-context:first" );
    first_request.set_sequence( firstClientSequence );
    eventgrab::v1::SetClientContextResponse first_response;
    grpc::ClientContext                     first_context;
    first_context.set_deadline( std::chrono::system_clock::now() + unaryDeadline );

    const auto first_status =
        server.stub().SetClientContext( &first_context, first_request, &first_response );
    ASSERT_TRUE( first_status.ok() ) << first_status.error_message();
    EXPECT_EQ( first_response.sequence(), first_request.sequence() );
    ASSERT_EQ( first_response.diagnostics().log_size(), 1 );
    const auto first_sequence = first_response.diagnostics().log( 0 ).sequence();
    EXPECT_EQ( first_sequence, first_request.sequence() );
    EXPECT_NE(
        first_response.diagnostics().log( 0 ).message().find( first_request.context() ),
        std::string::npos
    );

    eventgrab::v1::SetClientContextRequest second_request;
    second_request.set_context( "client-context:second" );
    second_request.set_sequence( secondClientSequence );
    eventgrab::v1::SetClientContextResponse second_response;
    grpc::ClientContext                     second_context;
    second_context.set_deadline( std::chrono::system_clock::now() + unaryDeadline );

    const auto second_status = server.stub().SetClientContext( &second_context,
                                                               second_request,
                                                               &second_response );
    ASSERT_TRUE( second_status.ok() ) << second_status.error_message();
    EXPECT_EQ( second_response.sequence(), second_request.sequence() );
    ASSERT_EQ( second_response.diagnostics().log_size(), 1 );
    EXPECT_EQ( second_response.diagnostics().log( 0 ).sequence(),
               second_request.sequence() );
    EXPECT_GT( second_response.diagnostics().log( 0 ).sequence(), first_sequence );
    EXPECT_NE( second_response.diagnostics().log( 0 ).message().find(
                   second_request.context()
               ),
               std::string::npos );
}
