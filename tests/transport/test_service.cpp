#include "eventgrab/v1/events.pb.h"
#include "eventgrab/v1/service.grpc.pb.h"
#include "eventgrab/v1/service.pb.h"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "transport/codec.hpp"
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
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
// clang-format on

namespace
{

    constexpr std::string_view kUnixEndpointPrefix  = "unix:";
    constexpr std::string_view kSocketStem          = "grab-event-service-";
    constexpr std::string_view kNameSeparator       = "-";
    constexpr std::string_view kUnknownTestName     = "unknown";
    constexpr std::string_view kSocketExtension     = ".sock";
    constexpr auto             kUnaryDeadline       = std::chrono::seconds{ 2 };
    constexpr auto             kStreamDeadline      = std::chrono::seconds{ 5 };
    constexpr auto             kStreamReadyTimeout  = std::chrono::seconds{ 2 };
    constexpr auto             kStreamResultTimeout = std::chrono::seconds{ 2 };
    constexpr auto             kNoEventWindow       = std::chrono::milliseconds{ 200 };
    constexpr double           kEventTimestamp      = 1729.25;
    constexpr std::uint64_t    kNoSequence          = 0U;
    constexpr std::uint32_t    kKeyDownCode         = 30U;
    constexpr std::uint32_t    kKeyUpCode           = 31U;
    constexpr std::string_view kKeyDownName         = "A";
    constexpr std::string_view kKeyUpName           = "B";
    constexpr auto             kInvalidArgumentCode = grpc::StatusCode::INVALID_ARGUMENT;
    constexpr auto             kCancelledCode       = grpc::StatusCode::CANCELLED;

    [[nodiscard]]
    std::string
    current_test_socket_name()
    {
        const auto* const test_info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        if( test_info == nullptr )
        {
            return std::string{ kSocketStem } +
                   std::string{ kUnknownTestName } +
                   std::string{ kSocketExtension };
        }

        return std::string{ kSocketStem } +
               test_info->test_suite_name() +
               std::string{ kNameSeparator } +
               test_info->name() +
               std::string{ kSocketExtension };
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
                endpoint_( std::string{ kUnixEndpointPrefix } + path_.string() )
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
            .timestamp = kEventTimestamp,
            .sequence  = kNoSequence,
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
            make_key_event( grab::EventKind::key_down, kKeyDownCode, kKeyDownName )
        );
    }

    [[nodiscard]]
    eventgrab::v1::Event
    key_up_wire()
    {
        return make_wire_event(
            make_key_event( grab::EventKind::key_up, kKeyUpCode, kKeyUpName )
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

            explicit TestServer( grab::EventBus& bus )
            {
                auto transport =
                    grab::transport::TransportServer::start( endpoint_.value(), bus );
                if( transport.has_value() )
                {
                    transport_.emplace( std::move( *transport ) );
                    stub_ = make_stub( endpoint_.value() );
                    return;
                }

                service_ = std::make_unique<grab::transport::EventService>( bus );
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
        context.set_deadline( std::chrono::system_clock::now() + kUnaryDeadline );

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
                                       kStreamDeadline );

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
        if( subscriber.result.wait_for( kStreamResultTimeout ) !=
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
    expect_type_info( const eventgrab::v1::ListEventTypesResponse& response,
                      eventgrab::v1::EventKind                     kind,
                      eventgrab::v1::EventCategory                 category )
    {
        const auto* type = find_type( response, kind );
        ASSERT_NE( type, nullptr );
        EXPECT_EQ( type->category(), category );
        EXPECT_EQ( type->name(), eventgrab::v1::EventKind_Name( kind ) );
        EXPECT_FALSE( type->active() );
    }

    [[nodiscard]]
    bool
    stream_finished_cleanly( const grpc::Status& status ) noexcept
    {
        return status.ok() || status.error_code() == kCancelledCode;
    }

}    // namespace

TEST( EventService,
      PushedEventReachesSubscriber )
{
    grab::EventBus   bus;
    const TestServer server{ bus };
    ASSERT_TRUE( server.started() );

    const eventgrab::v1::EventFilter filter;
    auto subscriber = start_subscribe( server.stub(), filter );
    ASSERT_EQ( subscriber.ready.wait_for( kStreamReadyTimeout ),
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
    ASSERT_EQ( subscriber.ready.wait_for( kStreamReadyTimeout ),
               std::future_status::ready );

    eventgrab::v1::Event malformed;
    malformed.set_kind( eventgrab::v1::EVENT_KIND_UNSPECIFIED );
    malformed.set_category( eventgrab::v1::EVENT_CATEGORY_INPUT );
    malformed.set_timestamp( kEventTimestamp );

    const auto status = push_wire_event( server.stub(), malformed );
    EXPECT_EQ( status.error_code(), kInvalidArgumentCode );
    EXPECT_EQ( subscriber.result.wait_for( kNoEventWindow ),
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
    ASSERT_EQ( subscriber.ready.wait_for( kStreamReadyTimeout ),
               std::future_status::ready );

    const auto ignored_status = push_wire_event( server.stub(), key_up_wire() );
    ASSERT_TRUE( ignored_status.ok() ) << ignored_status.error_message();
    EXPECT_EQ( subscriber.result.wait_for( kNoEventWindow ),
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

    grpc::ClientContext context;
    context.set_deadline( std::chrono::system_clock::now() + kUnaryDeadline );

    const eventgrab::v1::ListEventTypesRequest request;
    eventgrab::v1::ListEventTypesResponse      response;
    const auto status = server.stub().ListEventTypes( &context, request, &response );

    ASSERT_TRUE( status.ok() ) << status.error_message();
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
      ClientCancelStopsStreamCleanly )
{
    grab::EventBus bus;
    TestServer     server{ bus };
    ASSERT_TRUE( server.started() );

    const eventgrab::v1::EventFilter filter;
    auto subscriber = start_subscribe( server.stub(), filter );
    ASSERT_EQ( subscriber.ready.wait_for( kStreamReadyTimeout ),
               std::future_status::ready );

    subscriber.cancel();
    const auto result = finish_subscription( subscriber );
    EXPECT_FALSE( result.got_event );
    EXPECT_TRUE( stream_finished_cleanly( result.status ) );

    server.shutdown();
}
