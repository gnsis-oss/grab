#include "frontends/grpc/service.hpp"
#include "grab/event_bus.hpp"
#include "grab/process_ref.hpp"

#include <google/protobuf/descriptor.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/server_builder.h>

// clang-format off
#include <gtest/gtest.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <thread>
#include <utility>
#include <vector>
// clang-format on

namespace
{

    using namespace std::chrono_literals;

    constexpr int  processExitProbeAttempts = 200;
    constexpr auto processExitProbeInterval = std::chrono::milliseconds{ 5 };

    [[nodiscard]]
    grab::transport::ServiceOptions
    constrained_options()
    {
        grab::transport::ServiceOptions options;
        options.poll_interval               = 5ms;
        options.admission.concurrency_cap   = 1U;
        options.admission.queue_capacity    = 0U;
        options.admission.per_call_deadline = 2s;
        options.admission.unhealthy_reason  = "daemon health rejected the call";
        return options;
    }

    class ConstrainedServer
    {
        public:

            ConstrainedServer() :
                options_{ constrained_options() },
                service_{
                    bus_,
                    nullptr,
                    options_
                }
            {
                grpc::ServerBuilder builder;
                int                 port{};
                builder.AddListeningPort( "127.0.0.1:0",
                                          grpc::InsecureServerCredentials(),
                                          &port );
                builder.RegisterService( &service_ );
                server_ = builder.BuildAndStart();
                if( server_ != nullptr )
                {
                    const auto endpoint =
                        std::string{ "127.0.0.1:" } + std::to_string( port );
                    stub_ = eventgrab::v1::EventGrabService::NewStub(
                        grpc::CreateChannel( endpoint,
                                             grpc::InsecureChannelCredentials() )
                    );
                }
            }

            ~ConstrainedServer()
            {
                if( server_ != nullptr )
                {
                    server_->Shutdown();
                }
            }

            [[nodiscard]]
            bool
            started() const noexcept
            {
                return server_ != nullptr && stub_ != nullptr;
            }

            [[nodiscard]]
            eventgrab::v1::EventGrabService::Stub&
            stub() const
            {
                return *stub_;
            }

        private:

            grab::EventBus                                         bus_;
            grab::transport::ServiceOptions                        options_;
            grab::transport::EventService                          service_;
            std::unique_ptr<grpc::Server>                          server_;
            std::unique_ptr<eventgrab::v1::EventGrabService::Stub> stub_;
    };

}    // namespace

TEST( EventServiceHardening,
      EveryRegisteredRpcIsAdmissionWrappedExactlyOnce )
{
    grab::EventBus                bus;
    grab::transport::EventService service{ bus };

    const auto* pool = google::protobuf::DescriptorPool::generated_pool();
    const auto* svc  = pool->FindServiceByName(
        std::string{ eventgrab::v1::EventGrabService::service_full_name() }
    );
    ASSERT_NE( svc, nullptr );
    EXPECT_EQ( service.registered_rpc_count(),
               static_cast<std::size_t>( svc->method_count() ) );
    for( int index = 0; index < svc->method_count(); ++index )
    {
        const std::string name = svc->method( index )->name();
        EXPECT_EQ( service.wrapped_rpc_count( name ), 1U ) << name;
    }

    {
        eventgrab::v1::PushEventRequest  request;
        eventgrab::v1::PushEventResponse response;
        grpc::ServerContext              context;
        static_cast<void>( service.PushEvent( &context, &request, &response ) );
    }
    {
        eventgrab::v1::ListEventTypesRequest  request;
        eventgrab::v1::ListEventTypesResponse response;
        grpc::ServerContext                   context;
        static_cast<void>( service.ListEventTypes( &context, &request, &response ) );
    }
    {
        eventgrab::v1::EventFilter request;
        grpc::ServerContext        context;
        static_cast<void>( service.Subscribe( &context, &request, nullptr ) );
    }
    {
        eventgrab::v1::SetClientContextRequest  request;
        eventgrab::v1::SetClientContextResponse response;
        grpc::ServerContext                     context;
        static_cast<void>( service.SetClientContext( &context, &request, &response ) );
    }
    {
        eventgrab::v1::ResolveNodeRequest  request;
        eventgrab::v1::ResolveNodeResponse response;
        grpc::ServerContext                context;
        static_cast<void>( service.ResolveNode( &context, &request, &response ) );
    }
    {
        eventgrab::v1::PerformActionRequest  request;
        eventgrab::v1::PerformActionResponse response;
        grpc::ServerContext                  context;
        static_cast<void>( service.PerformAction( &context, &request, &response ) );
    }
    {
        eventgrab::v1::CaptureFrameRequest  request;
        eventgrab::v1::CaptureFrameResponse response;
        grpc::ServerContext                 context;
        static_cast<void>( service.CaptureFrame( &context, &request, &response ) );
    }

    for( int index = 0; index < svc->method_count(); ++index )
    {
        const std::string name = svc->method( index )->name();
        EXPECT_EQ( service.admission_entry_count( name ), 1U )
            << name
            << " did not route through the single admission dispatcher exactly once";
    }
}

TEST( EventServiceHardening,
      ConcurrencyRejectionCancelsWithoutOrphaningTheSlot )
{
    const ConstrainedServer server;
    ASSERT_TRUE( server.started() );

    grpc::ClientContext              held_context;
    const eventgrab::v1::EventFilter filter;
    auto held = server.stub().Subscribe( &held_context, filter );
    ASSERT_NE( held, nullptr );

    grpc::Status rejected;
    for( std::size_t attempt{}; attempt < 50U; ++attempt )
    {
        grpc::ClientContext                   context;
        eventgrab::v1::ListEventTypesRequest  request;
        eventgrab::v1::ListEventTypesResponse response;
        rejected = server.stub().ListEventTypes( &context, request, &response );
        if( rejected.error_code() == grpc::StatusCode::RESOURCE_EXHAUSTED )
        {
            break;
        }
        std::this_thread::sleep_for( 2ms );
    }

    EXPECT_EQ( rejected.error_code(), grpc::StatusCode::RESOURCE_EXHAUSTED );
    EXPECT_NE( rejected.error_message().find( "concurrency" ), std::string::npos );
    EXPECT_NE( rejected.error_message(),
               constrained_options().admission.unhealthy_reason );

    held_context.TryCancel();
    static_cast<void>( held->Finish() );

    grpc::Status admitted;
    for( std::size_t attempt{}; attempt < 50U; ++attempt )
    {
        grpc::ClientContext                   context;
        eventgrab::v1::ListEventTypesRequest  request;
        eventgrab::v1::ListEventTypesResponse response;
        admitted = server.stub().ListEventTypes( &context, request, &response );
        if( admitted.ok() )
        {
            break;
        }
        std::this_thread::sleep_for( 2ms );
    }
    EXPECT_TRUE( admitted.ok() ) << admitted.error_message();
}

TEST( EventServiceHardening,
      HealthRejectionUsesItsDistinctHumanReadableReason )
{
    const std::atomic_bool unhealthy{ false };
    auto                   options = constrained_options();
    options.admission.healthy      = &unhealthy;
    grab::EventBus                             bus;
    grab::transport::EventService              service{ bus, nullptr, options };

    grpc::ServerContext                        context;
    const eventgrab::v1::ListEventTypesRequest request;
    eventgrab::v1::ListEventTypesResponse      response;
    const auto status = service.ListEventTypes( &context, &request, &response );

    EXPECT_EQ( status.error_code(), grpc::StatusCode::UNAVAILABLE );
    EXPECT_EQ( status.error_message(), options.admission.unhealthy_reason );
}

TEST( PeerSessionRegistry,
      AdoptionRequiresTheSessionToken )
{
    std::size_t teardown_count{};
    auto        teardown = [&]( std::string_view, const std::vector<std::string>& )
    {
        ++teardown_count;
    };
    grab::transport::PeerSessionRegistry registry{ teardown };

    const auto                           credentials = registry.open( "original-peer" );
    auto                                 wrong_token = credentials.token;
    wrong_token.append( "-not-the-token" );

    const auto rejected =
        registry.adopt( "adopting-peer", credentials.session, wrong_token );
    EXPECT_EQ( rejected.error_code(), grpc::StatusCode::UNAUTHENTICATED );
    EXPECT_FALSE( registry.active( "adopting-peer" ) );

    const auto admitted =
        registry.adopt( "adopting-peer", credentials.session, credentials.token );
    EXPECT_TRUE( admitted.ok() ) << admitted.error_message();
    EXPECT_TRUE( registry.active( "adopting-peer" ) );
    EXPECT_EQ( teardown_count, 0U );
}

TEST( PeerSessionRegistry,
      DeliberateAndCrashCloseShareTheExactlyOncePath )
{
    std::vector<std::pair<std::string, std::vector<std::string>>> teardowns;
    auto                                                          teardown =
        [&]( std::string_view peer, const std::vector<std::string>& resources )
    {
        teardowns.emplace_back( peer, resources );
    };
    grab::transport::PeerSessionRegistry registry{ teardown };

    static_cast<void>( registry.open( "deliberate-peer" ) );
    ASSERT_TRUE(
        registry.add_resource( "deliberate-peer", "owned-deliberate-resource" ).ok()
    );
    registry.deliberate_close( "deliberate-peer" );
    registry.deliberate_close( "deliberate-peer" );
    EXPECT_FALSE( registry.active( "deliberate-peer" ) );
    EXPECT_EQ( registry.close_count(), 1U );
    ASSERT_EQ( teardowns.size(), 1U );
    EXPECT_EQ( teardowns.front().first, "deliberate-peer" );
    EXPECT_EQ( teardowns.front().second,
               std::vector<std::string>{ "owned-deliberate-resource" } );

    static_cast<void>( registry.open( "crashed-peer" ) );
    ASSERT_TRUE( registry.add_resource( "crashed-peer", "owned-crash-resource" ).ok() );
    registry.unexpected_disconnect( "crashed-peer" );
    registry.unexpected_disconnect( "crashed-peer" );
    EXPECT_FALSE( registry.active( "crashed-peer" ) );
    EXPECT_EQ( registry.close_count(), 2U );
    ASSERT_EQ( teardowns.size(), 2U );
    EXPECT_EQ( teardowns.back().first, "crashed-peer" );
    EXPECT_EQ( teardowns.back().second,
               std::vector<std::string>{ "owned-crash-resource" } );
}

TEST( PeerSessionRegistry,
      DeliberateCloseTerminatesOwnedProcesses )
{
    grab::transport::PeerSessionRegistry registry;
    static_cast<void>( registry.open( "proc-peer" ) );

    auto child = grab::OwnedProcess::spawn(
        std::to_array<std::string_view>( { "sleep", "1000" } )
    );
    ASSERT_TRUE( child.has_value() ) << child.error().message;
    const auto pid = child->id().value;
    ASSERT_TRUE(
        registry.add_process( "proc-peer", "child", std::move( *child ) ).ok()
    );

    registry.deliberate_close( "proc-peer" );

    bool gone = false;
    for( int attempt = 0; attempt < processExitProbeAttempts && !gone; ++attempt )
    {
        // NOLINTNEXTLINE(misc-include-cleaner): POSIX kill is exposed by <csignal>.
        if( ::kill( static_cast<pid_t>( pid ), 0 ) != 0 && errno == ESRCH )
        {
            gone = true;
            break;
        }
        std::this_thread::sleep_for( processExitProbeInterval );
    }
    EXPECT_TRUE( gone ) << "owned child was leaked alive after session close";
}

TEST( PeerSessionRegistry,
      CatalogScopeReapsOnlyResourcesCreatedInsideTheScope )
{
    std::vector<std::vector<std::string>> reaped;
    auto teardown = [&]( std::string_view, const std::vector<std::string>& resources )
    {
        reaped.push_back( resources );
    };
    grab::transport::PeerSessionRegistry registry{ teardown };
    static_cast<void>( registry.open( "scoped-peer" ) );
    ASSERT_TRUE( registry.add_resource( "scoped-peer", "preexisting" ).ok() );

    {
        auto scope = registry.scope( "scoped-peer" );
        ASSERT_TRUE( registry.add_resource( "scoped-peer", "created-here" ).ok() );
    }

    ASSERT_EQ( reaped.size(), 1U );
    EXPECT_EQ( reaped.front(), std::vector<std::string>{ "created-here" } );
    EXPECT_TRUE( registry.active( "scoped-peer" ) );
}
