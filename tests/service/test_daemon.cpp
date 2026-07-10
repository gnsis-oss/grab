#include "event/fake_source.hpp"
#include "event/source.hpp"
#include "eventgrab/v1/events.pb.h"
#include "eventgrab/v1/service.grpc.pb.h"
#include "eventgrab/v1/service.pb.h"
#include "grab/event.hpp"
#include "grab/result.hpp"
#include "service/daemon.hpp"
#include "transport/codec.hpp"

#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/support/status.h>
#include <grpcpp/support/status_code_enum.h>

// clang-format off
#include <gtest/gtest.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
// clang-format on

namespace
{

    constexpr std::string_view unixEndpointPrefix      = "unix:";
    constexpr std::string_view tempRootName            = "grab-daemon-tests";
    constexpr std::string_view nameSeparator           = "-";
    constexpr std::string_view unknownTestName         = "unknown";
    constexpr std::string_view socketFileName          = "daemon.sock";
    constexpr std::string_view storeDirName            = "store";
    constexpr std::string_view jsonlExtension          = ".jsonl";
    constexpr std::string_view persistedTypeNeedle     = R"("type":"input.key_down")";
    constexpr std::string_view persistedCategoryNeedle = R"("category":"input")";
    constexpr std::string_view persistedKeyCodeNeedle  = R"("key_code":30)";
    constexpr std::string_view persistedKeyNameNeedle  = R"("key_name":"A")";
    constexpr std::string_view transportStartFailurePrefix =
        "failed to start transport server at unix:";
    constexpr auto             unaryDeadline       = std::chrono::seconds{ 2 };
    constexpr auto             shutdownDeadline    = std::chrono::milliseconds{ 500 };
    constexpr auto             streamDeadline      = std::chrono::seconds{ 5 };
    constexpr auto             streamReadyTimeout  = std::chrono::seconds{ 2 };
    constexpr auto             streamResultTimeout = std::chrono::seconds{ 2 };
    constexpr auto             persistencePollInterval = std::chrono::milliseconds{ 20 };
    constexpr std::size_t      persistenceAttempts     = 100U;
    constexpr double           eventTimestamp          = 1'704'067'200.0;
    constexpr std::uint64_t    noSequence              = 0U;
    constexpr std::uint32_t    keyDownCode             = 30U;
    constexpr std::string_view keyDownName             = "A";
    constexpr auto             cancelledCode           = grpc::StatusCode::CANCELLED;
    constexpr std::string_view firstSourceName         = "first";
    constexpr std::string_view secondSourceName        = "second";
    constexpr std::string_view failingSourceName       = "failing";
    constexpr std::string_view sourceFailureMessage    = "source failed";

    [[nodiscard]]
    std::string
    current_test_name()
    {
        const auto* const test_info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        if( test_info == nullptr )
        {
            return std::string{ unknownTestName };
        }

        return test_info->test_suite_name() +
               std::string{ nameSeparator } +
               test_info->name();
    }

    class TempDaemonDir
    {
        public:

            TempDaemonDir() :
                root_( std::filesystem::temp_directory_path() /
                       std::string{ tempRootName } /
                       current_test_name() ),
                store_( root_ / std::string{ storeDirName } ),
                socket_( root_ / std::string{ socketFileName } ),
                endpoint_( std::string{ unixEndpointPrefix } + socket_.string() )
            {
                std::error_code ec;
                const auto      removed = std::filesystem::remove_all( root_, ec );
                static_cast<void>( removed );
                const bool created = std::filesystem::create_directories( root_, ec );
                static_cast<void>( created );
            }

            ~TempDaemonDir() noexcept
            {
                std::error_code ec;
                const auto      removed = std::filesystem::remove_all( root_, ec );
                static_cast<void>( removed );
            }

            TempDaemonDir( const TempDaemonDir& ) = delete;
            TempDaemonDir&
            operator=( const TempDaemonDir& ) = delete;
            TempDaemonDir( TempDaemonDir&& )  = delete;
            TempDaemonDir&
            operator=( TempDaemonDir&& ) = delete;

            [[nodiscard]]
            const std::filesystem::path&
            store_dir() const noexcept
            {
                return store_;
            }

            [[nodiscard]]
            const std::string&
            endpoint() const noexcept
            {
                return endpoint_;
            }

        private:

            std::filesystem::path root_;
            std::filesystem::path store_;
            std::filesystem::path socket_;
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

    template<typename T>
    [[nodiscard]]
    testing::AssertionResult
    is_ok( const grab::Result<T>& result )
    {
        if( result.has_value() )
        {
            return testing::AssertionSuccess();
        }
        return testing::AssertionFailure() << result.error().message;
    }

    template<typename T>
    [[nodiscard]]
    bool
    transport_start_blocked( const grab::Result<T>& result )
    {
        return !result.has_value() &&
               result.error().message.starts_with( transportStartFailurePrefix );
    }

    [[nodiscard]]
    grab::Event
    key_down_event()
    {
        return grab::Event{
            .timestamp = eventTimestamp,
            .sequence  = noSequence,
            .kind      = grab::EventKind::KeyDown,
            .category  = grab::EventCategory::Input,
            .payload   = grab::Payload{ grab::InputKey{
                .code = keyDownCode,
                .name = std::string{ keyDownName },
            } },
        };
    }

    [[nodiscard]]
    eventgrab::v1::Event
    key_down_wire()
    {
        auto wire = grab::transport::to_wire( key_down_event() );
        EXPECT_TRUE( wire.has_value() );
        if( !wire.has_value() )
        {
            return {};
        }
        return *wire;
    }

    [[nodiscard]]
    grab::Result<void>
    failed_source_start()
    {
        return grab::fail( grab::ErrorCode::ProviderFailed,
                           std::string{ sourceFailureMessage } );
    }

    [[nodiscard]]
    std::unique_ptr<grab::test::FakeSource>
    make_source( std::string                  name,
                 std::vector<grab::EventKind> kinds = {} )
    {
        return std::make_unique<grab::test::FakeSource>( std::move( name ),
                                                         std::move( kinds ) );
    }

    [[nodiscard]]
    std::vector<std::unique_ptr<grab::event::EventSource>>
    make_source_list( std::vector<grab::test::FakeSource*>& sources_out )
    {
        auto first  = make_source( std::string{ firstSourceName } );
        auto second = make_source( std::string{ secondSourceName } );

        sources_out = { first.get(), second.get() };

        std::vector<std::unique_ptr<grab::event::EventSource>> sources;
        sources.emplace_back( std::move( first ) );
        sources.emplace_back( std::move( second ) );
        return sources;
    }

    [[nodiscard]]
    std::vector<std::unique_ptr<grab::event::EventSource>>
    make_degraded_source_list( std::vector<grab::test::FakeSource*>& sources_out )
    {
        auto first   = make_source( std::string{ firstSourceName } );
        auto failing = make_source( std::string{ failingSourceName } );
        auto second  = make_source( std::string{ secondSourceName } );
        failing->set_start_result( failed_source_start() );

        sources_out = { first.get(), failing.get(), second.get() };

        std::vector<std::unique_ptr<grab::event::EventSource>> sources;
        sources.emplace_back( std::move( first ) );
        sources.emplace_back( std::move( failing ) );
        sources.emplace_back( std::move( second ) );
        return sources;
    }

    [[nodiscard]]
    std::vector<std::unique_ptr<grab::event::EventSource>>
    make_key_down_source_list()
    {
        std::vector<std::unique_ptr<grab::event::EventSource>> sources;
        sources.emplace_back( make_source( std::string{ firstSourceName },
                                           { grab::EventKind::KeyDown } ) );
        return sources;
    }

    [[nodiscard]]
    std::unique_ptr<eventgrab::v1::EventGrabService::Stub>
    make_stub( const std::string& endpoint )
    {
        auto channel =
            grpc::CreateChannel( endpoint, grpc::InsecureChannelCredentials() );
        return eventgrab::v1::EventGrabService::NewStub( channel );
    }

    [[nodiscard]]
    grpc::Status
    push_wire_event( eventgrab::v1::EventGrabService::Stub& stub,
                     const eventgrab::v1::Event&            wire,
                     std::chrono::system_clock::duration    deadline )
    {
        grpc::ClientContext context;
        context.set_deadline( std::chrono::system_clock::now() + deadline );

        eventgrab::v1::PushEventRequest  request;
        eventgrab::v1::PushEventResponse response;
        *request.mutable_event() = wire;
        return stub.PushEvent( &context, request, &response );
    }

    [[nodiscard]]
    grpc::Status
    push_wire_event( eventgrab::v1::EventGrabService::Stub& stub,
                     const eventgrab::v1::Event&            wire )
    {
        return push_wire_event( stub, wire, unaryDeadline );
    }

    [[nodiscard]]
    RunningSubscribe
    start_subscribe( eventgrab::v1::EventGrabService::Stub& stub )
    {
        RunningSubscribe running;
        running.context = std::make_unique<grpc::ClientContext>();
        running.context->set_deadline( std::chrono::system_clock::now() +
                                       streamDeadline );

        eventgrab::v1::EventFilter     filter;
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

    [[nodiscard]]
    bool
    stream_finished_cleanly( const grpc::Status& status ) noexcept
    {
        return status.ok() || status.error_code() == cancelledCode;
    }

    [[nodiscard]]
    bool
    line_contains_persisted_event( std::string_view line )
    {
        return line.contains( persistedTypeNeedle ) &&
               line.contains( persistedCategoryNeedle ) &&
               line.contains( persistedKeyCodeNeedle ) &&
               line.contains( persistedKeyNameNeedle );
    }

    [[nodiscard]]
    bool
    file_contains_persisted_event( const std::filesystem::path& file )
    {
        std::ifstream input( file );
        std::string   line;
        while( std::getline( input, line ) )
        {
            if( line_contains_persisted_event( line ) )
            {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]]
    bool
    persisted_event_present( const std::filesystem::path& dir )
    {
        std::error_code ec;
        const bool      exists = std::filesystem::exists( dir, ec );
        if( ec || !exists )
        {
            return false;
        }

        auto iter = std::filesystem::directory_iterator( dir, ec );
        const std::filesystem::directory_iterator end;
        while( !ec && iter != end )
        {
            const auto path = iter->path();
            if( path.extension() ==
                jsonlExtension &&
                file_contains_persisted_event( path ) )
            {
                return true;
            }
            iter.increment( ec );
        }
        return false;
    }

    [[nodiscard]]
    bool
    wait_for_persisted_event( const std::filesystem::path& dir )
    {
        for( std::size_t attempt = 0U; attempt < persistenceAttempts; ++attempt )
        {
            if( persisted_event_present( dir ) )
            {
                return true;
            }
            std::this_thread::sleep_for( persistencePollInterval );
        }
        return false;
    }

}    // namespace

TEST( Daemon,
      PushedEventIsBroadcastAndPersisted )
{
    const TempDaemonDir temp;
    auto daemon_result = grab::service::Daemon::start( grab::service::DaemonOptions{
        .endpoint       = temp.endpoint(),
        .store_dir      = temp.store_dir(),
        .source_factory = {},
    } );
    if( transport_start_blocked( daemon_result ) )
    {
        GTEST_SKIP() << daemon_result.error().message;
    }
    ASSERT_TRUE( is_ok( daemon_result ) );
    auto daemon     = std::move( daemon_result ).value();
    auto stub       = make_stub( daemon.endpoint() );

    auto subscriber = start_subscribe( *stub );
    ASSERT_EQ( subscriber.ready.wait_for( streamReadyTimeout ),
               std::future_status::ready );

    const auto expected = key_down_wire();
    const auto status   = push_wire_event( *stub, expected );
    ASSERT_TRUE( status.ok() ) << status.error_message();

    const auto result = finish_subscription( subscriber );
    ASSERT_TRUE( result.got_event );
    expect_wire_event_eq( expected, result.event );
    EXPECT_TRUE( stream_finished_cleanly( result.status ) );
    EXPECT_TRUE( wait_for_persisted_event( temp.store_dir() ) );

    daemon.shutdown();
}

TEST( Daemon,
      ShutdownIsCleanAndIdempotent )
{
    const TempDaemonDir temp;
    auto daemon_result = grab::service::Daemon::start( grab::service::DaemonOptions{
        .endpoint       = temp.endpoint(),
        .store_dir      = std::nullopt,
        .source_factory = {},
    } );
    if( transport_start_blocked( daemon_result ) )
    {
        GTEST_SKIP() << daemon_result.error().message;
    }
    ASSERT_TRUE( is_ok( daemon_result ) );
    auto daemon = std::move( daemon_result ).value();
    auto stub   = make_stub( daemon.endpoint() );

    daemon.shutdown();
    daemon.shutdown();

    const auto status = push_wire_event( *stub, key_down_wire(), shutdownDeadline );
    EXPECT_FALSE( status.ok() );
}

TEST( Daemon,
      StartsWithoutStorageWhenNoDir )
{
    const TempDaemonDir temp;
    auto daemon_result = grab::service::Daemon::start( grab::service::DaemonOptions{
        .endpoint       = temp.endpoint(),
        .store_dir      = std::nullopt,
        .source_factory = {},
    } );
    if( transport_start_blocked( daemon_result ) )
    {
        GTEST_SKIP() << daemon_result.error().message;
    }
    ASSERT_TRUE( is_ok( daemon_result ) );
    auto daemon     = std::move( daemon_result ).value();
    auto stub       = make_stub( daemon.endpoint() );

    auto subscriber = start_subscribe( *stub );
    ASSERT_EQ( subscriber.ready.wait_for( streamReadyTimeout ),
               std::future_status::ready );

    const auto expected = key_down_wire();
    const auto status   = push_wire_event( *stub, expected );
    ASSERT_TRUE( status.ok() ) << status.error_message();

    const auto result = finish_subscription( subscriber );
    ASSERT_TRUE( result.got_event );
    expect_wire_event_eq( expected, result.event );
    EXPECT_TRUE( stream_finished_cleanly( result.status ) );
    EXPECT_FALSE( std::filesystem::exists( temp.store_dir() ) );

    daemon.shutdown();
}

TEST( Daemon,
      StartsAndStopsInjectedSources )
{
    const TempDaemonDir                  temp;
    std::vector<grab::test::FakeSource*> sources;

    auto daemon_result = grab::service::Daemon::start( grab::service::DaemonOptions{
        .endpoint       = temp.endpoint(),
        .store_dir      = std::nullopt,
        .source_factory = [&sources]
        {
            return make_source_list( sources );
        },
    } );
    if( transport_start_blocked( daemon_result ) )
    {
        GTEST_SKIP() << daemon_result.error().message;
    }
    ASSERT_TRUE( is_ok( daemon_result ) );
    auto daemon = std::move( daemon_result ).value();

    ASSERT_EQ( sources.size(), 2U );
    EXPECT_EQ( sources.at( 0U )->state(), grab::event::SourceState::Running );
    EXPECT_EQ( sources.at( 1U )->state(), grab::event::SourceState::Running );
    EXPECT_EQ( sources.at( 0U )->start_calls(), 1U );
    EXPECT_EQ( sources.at( 1U )->start_calls(), 1U );

    daemon.shutdown();

    EXPECT_EQ( sources.at( 0U )->state(), grab::event::SourceState::Stopped );
    EXPECT_EQ( sources.at( 1U )->state(), grab::event::SourceState::Stopped );
    EXPECT_EQ( sources.at( 0U )->stop_calls(), 1U );
    EXPECT_EQ( sources.at( 1U )->stop_calls(), 1U );
}

TEST( Daemon,
      StartsWhenInjectedSourceFails )
{
    const TempDaemonDir                  temp;
    std::vector<grab::test::FakeSource*> sources;

    auto daemon_result = grab::service::Daemon::start( grab::service::DaemonOptions{
        .endpoint       = temp.endpoint(),
        .store_dir      = std::nullopt,
        .source_factory = [&sources]
        {
            return make_degraded_source_list( sources );
        },
    } );
    if( transport_start_blocked( daemon_result ) )
    {
        GTEST_SKIP() << daemon_result.error().message;
    }
    ASSERT_TRUE( is_ok( daemon_result ) );
    auto daemon = std::move( daemon_result ).value();

    ASSERT_EQ( sources.size(), 3U );
    EXPECT_EQ( sources.at( 0U )->state(), grab::event::SourceState::Running );
    EXPECT_EQ( sources.at( 1U )->state(), grab::event::SourceState::Failed );
    EXPECT_EQ( sources.at( 2U )->state(), grab::event::SourceState::Running );

    daemon.shutdown();

    EXPECT_EQ( sources.at( 0U )->state(), grab::event::SourceState::Stopped );
    EXPECT_EQ( sources.at( 1U )->state(), grab::event::SourceState::Failed );
    EXPECT_EQ( sources.at( 2U )->state(), grab::event::SourceState::Stopped );
}

TEST( Daemon,
      ListEventTypesReflectsInjectedSourceActivity )
{
    const TempDaemonDir temp;
    auto daemon_result = grab::service::Daemon::start( grab::service::DaemonOptions{
        .endpoint       = temp.endpoint(),
        .store_dir      = std::nullopt,
        .source_factory = []
        {
            return make_key_down_source_list();
        },
    } );
    if( transport_start_blocked( daemon_result ) )
    {
        GTEST_SKIP() << daemon_result.error().message;
    }
    ASSERT_TRUE( is_ok( daemon_result ) );
    auto                                  daemon = std::move( daemon_result ).value();
    auto                                  stub   = make_stub( daemon.endpoint() );

    eventgrab::v1::ListEventTypesResponse response;
    list_event_types_or_fail( *stub, response );

    const auto* key_down = find_type( response, eventgrab::v1::INPUT_KEY_DOWN );
    ASSERT_NE( key_down, nullptr );
    EXPECT_TRUE( key_down->active() );

    const auto* state_snapshot = find_type( response, eventgrab::v1::STATE_SNAPSHOT );
    ASSERT_NE( state_snapshot, nullptr );
    EXPECT_FALSE( state_snapshot->active() );

    daemon.shutdown();
}
