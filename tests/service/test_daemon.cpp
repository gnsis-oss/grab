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
// clang-format on

namespace
{

    constexpr std::string_view kUnixEndpointPrefix     = "unix:";
    constexpr std::string_view kTempRootName           = "grab-daemon-tests";
    constexpr std::string_view kNameSeparator          = "-";
    constexpr std::string_view kUnknownTestName        = "unknown";
    constexpr std::string_view kSocketFileName         = "daemon.sock";
    constexpr std::string_view kStoreDirName           = "store";
    constexpr std::string_view kJsonlExtension         = ".jsonl";
    constexpr std::string_view kPersistedTypeNeedle    = R"("type":"input.key_down")";
    constexpr std::string_view kPersistedKeyCodeNeedle = R"("code":30)";
    constexpr std::string_view kPersistedKeyNameNeedle = R"("name":"A")";
    constexpr std::string_view kTransportStartFailurePrefix =
        "failed to start transport server at unix:";
    constexpr auto          kUnaryDeadline           = std::chrono::seconds{ 2 };
    constexpr auto          kShutdownDeadline        = std::chrono::milliseconds{ 500 };
    constexpr auto          kStreamDeadline          = std::chrono::seconds{ 5 };
    constexpr auto          kStreamReadyTimeout      = std::chrono::seconds{ 2 };
    constexpr auto          kStreamResultTimeout     = std::chrono::seconds{ 2 };
    constexpr auto          kPersistencePollInterval = std::chrono::milliseconds{ 20 };
    constexpr std::size_t   kPersistenceAttempts     = 100U;
    constexpr double        kEventTimestamp          = 1'704'067'200.0;
    constexpr std::uint64_t kNoSequence              = 0U;
    constexpr std::uint32_t kKeyDownCode             = 30U;
    constexpr std::string_view kKeyDownName          = "A";
    constexpr auto             kCancelledCode        = grpc::StatusCode::CANCELLED;

    [[nodiscard]]
    std::string
    current_test_name()
    {
        const auto* const test_info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        if( test_info == nullptr )
        {
            return std::string{ kUnknownTestName };
        }

        return test_info->test_suite_name() +
               std::string{ kNameSeparator } +
               test_info->name();
    }

    class TempDaemonDir
    {
        public:

            TempDaemonDir() :
                root_( std::filesystem::temp_directory_path() /
                       std::string{ kTempRootName } /
                       current_test_name() ),
                store_( root_ / std::string{ kStoreDirName } ),
                socket_( root_ / std::string{ kSocketFileName } ),
                endpoint_( std::string{ kUnixEndpointPrefix } + socket_.string() )
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
               result.error().message.starts_with( kTransportStartFailurePrefix );
    }

    [[nodiscard]]
    grab::Event
    key_down_event()
    {
        return grab::Event{
            .timestamp = kEventTimestamp,
            .sequence  = kNoSequence,
            .kind      = grab::EventKind::key_down,
            .category  = grab::EventCategory::input,
            .payload   = grab::Payload{ grab::InputKey{
                .code = kKeyDownCode,
                .name = std::string{ kKeyDownName },
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
        return push_wire_event( stub, wire, kUnaryDeadline );
    }

    [[nodiscard]]
    RunningSubscribe
    start_subscribe( eventgrab::v1::EventGrabService::Stub& stub )
    {
        RunningSubscribe running;
        running.context = std::make_unique<grpc::ClientContext>();
        running.context->set_deadline( std::chrono::system_clock::now() +
                                       kStreamDeadline );

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
    bool
    stream_finished_cleanly( const grpc::Status& status ) noexcept
    {
        return status.ok() || status.error_code() == kCancelledCode;
    }

    [[nodiscard]]
    bool
    line_contains_persisted_event( std::string_view line )
    {
        return line.contains( kPersistedTypeNeedle ) &&
               line.contains( kPersistedKeyCodeNeedle ) &&
               line.contains( kPersistedKeyNameNeedle );
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
                kJsonlExtension &&
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
        for( std::size_t attempt = 0U; attempt < kPersistenceAttempts; ++attempt )
        {
            if( persisted_event_present( dir ) )
            {
                return true;
            }
            std::this_thread::sleep_for( kPersistencePollInterval );
        }
        return false;
    }

}    // namespace

TEST( Daemon,
      PushedEventIsBroadcastAndPersisted )
{
    const TempDaemonDir temp;
    auto daemon_result = grab::service::Daemon::start( grab::service::DaemonOptions{
        .endpoint  = temp.endpoint(),
        .store_dir = temp.store_dir(),
    } );
    if( transport_start_blocked( daemon_result ) )
    {
        GTEST_SKIP() << daemon_result.error().message;
    }
    ASSERT_TRUE( is_ok( daemon_result ) );
    auto daemon     = std::move( daemon_result ).value();
    auto stub       = make_stub( daemon.endpoint() );

    auto subscriber = start_subscribe( *stub );
    ASSERT_EQ( subscriber.ready.wait_for( kStreamReadyTimeout ),
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
        .endpoint  = temp.endpoint(),
        .store_dir = std::nullopt,
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

    const auto status = push_wire_event( *stub, key_down_wire(), kShutdownDeadline );
    EXPECT_FALSE( status.ok() );
}

TEST( Daemon,
      StartsWithoutStorageWhenNoDir )
{
    const TempDaemonDir temp;
    auto daemon_result = grab::service::Daemon::start( grab::service::DaemonOptions{
        .endpoint  = temp.endpoint(),
        .store_dir = std::nullopt,
    } );
    if( transport_start_blocked( daemon_result ) )
    {
        GTEST_SKIP() << daemon_result.error().message;
    }
    ASSERT_TRUE( is_ok( daemon_result ) );
    auto daemon     = std::move( daemon_result ).value();
    auto stub       = make_stub( daemon.endpoint() );

    auto subscriber = start_subscribe( *stub );
    ASSERT_EQ( subscriber.ready.wait_for( kStreamReadyTimeout ),
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
