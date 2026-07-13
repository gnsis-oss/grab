#include "client/client.hpp"
#include "client/loopback_transport.hpp"
#include "client/unix_socket_transport.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/event_descriptor.hpp"
#include "grab/result.hpp"
#include "grab/trace.hpp"
#include "transport/server.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <variant>
// clang-format on

namespace
{

    constexpr std::string_view unixEndpointPrefix = "unix:";
    constexpr std::string_view transportStartFailurePrefix =
        "failed to start transport server at unix:";

    class TempSocket
    {
        public:

            TempSocket() :
                root_( std::filesystem::temp_directory_path() /
                       ( "grab-client-test-" +
                         std::to_string( static_cast<std::uint64_t>( getpid() ) ) ) ),
                endpoint_( std::string{ unixEndpointPrefix } +
                           ( root_ / "transport.sock" ).string() )
            {
                std::error_code ec;
                static_cast<void>( std::filesystem::remove_all( root_, ec ) );
                static_cast<void>( std::filesystem::create_directories( root_, ec ) );
            }

            ~TempSocket()
            {
                std::error_code ec;
                static_cast<void>( std::filesystem::remove_all( root_, ec ) );
            }

            TempSocket( const TempSocket& ) = delete;
            TempSocket&
            operator=( const TempSocket& ) = delete;
            TempSocket( TempSocket&& )     = delete;
            TempSocket&
            operator=( TempSocket&& ) = delete;

            [[nodiscard]]
            const std::string&
            endpoint() const noexcept
            {
                return endpoint_;
            }

        private:

            std::filesystem::path root_;
            std::string           endpoint_;
    };

    TEST( ClientLoopbackTransport,
          PushDeliversToInProcessSubscriber )
    {
        grab::EventBus                  bus;
        grab::client::LoopbackTransport transport{ bus };
        grab::client::Client            client{ transport };

        auto subscription = client.subscribe( grab::EventFilter{} );
        ASSERT_TRUE( subscription.has_value() );
        auto stream = std::move( *subscription );
        ASSERT_NE( stream, nullptr );

        const grab::Event expected{
            .timestamp = 42.5,
            .sequence  = 7U,
            .kind      = grab::EventKind::KeyDown,
            .category  = grab::EventCategory::Input,
            .payload   = grab::InputKey{ .code = 30U, .name = "a" },
        };

        auto pushed = client.push_event( expected );
        ASSERT_TRUE( pushed.has_value() );

        auto next = stream->try_next();
        ASSERT_TRUE( next.has_value() );
        ASSERT_TRUE( next->has_value() );
        ASSERT_TRUE( std::holds_alternative<grab::Event>( **next ) );
        const auto& received = std::get<grab::Event>( **next );
        EXPECT_EQ( received.kind, expected.kind );
        EXPECT_EQ( received.category, expected.category );
        EXPECT_DOUBLE_EQ( received.timestamp, expected.timestamp );
        EXPECT_GT( received.sequence, 0U );
    }

    TEST( ClientLoopbackTransport,
          ListEventTypesReturnsDescriptorSet )
    {
        grab::EventBus                  bus;
        grab::client::LoopbackTransport transport{ bus };
        grab::client::Client            client{ transport };

        auto                            descriptors = client.list_event_types();
        ASSERT_TRUE( descriptors.has_value() );
        EXPECT_EQ( descriptors->size(), grab::detail::eventDescriptors.size() - 1U );

        const auto key_down = std::ranges::find( *descriptors,
                                                 grab::EventKind::KeyDown,
                                                 &grab::EventTypeDescriptor::kind );
        ASSERT_NE( key_down, descriptors->end() );
        EXPECT_EQ( key_down->category, grab::EventCategory::Input );
        EXPECT_EQ( key_down->name, "input.key_down" );
        EXPECT_FALSE( key_down->active );
    }

    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    TEST( UnixSocketTransport,
          RoundTripsThroughTransportServer )
    {
        const TempSocket temp;
        grab::EventBus   bus;
        auto server = grab::transport::TransportServer::start( temp.endpoint(), bus );
        if( !server.has_value() &&
            server.error().message.starts_with( transportStartFailurePrefix ) )
        {
            GTEST_SKIP() << server.error().message;
        }
        ASSERT_TRUE( server.has_value() ) << server.error().message;

        auto bus_subscription = bus.subscribe( grab::EventFilter{} );
        grab::client::UnixSocketTransport transport{ temp.endpoint() };
        grab::client::Client              client{ transport };

        auto                              descriptors = client.list_event_types();
        ASSERT_TRUE( descriptors.has_value() ) << descriptors.error().message;
        EXPECT_EQ( descriptors->size(), grab::detail::eventDescriptors.size() - 1U );

        const grab::Event expected{
            .timestamp = 84.25,
            .sequence  = 0U,
            .kind      = grab::EventKind::KeyDown,
            .category  = grab::EventCategory::Input,
            .payload   = grab::InputKey{ .code = 42U, .name = "shift" },
        };
        auto pushed = client.push_event( expected );
        ASSERT_TRUE( pushed.has_value() ) << pushed.error().message;

        auto received = bus_subscription.try_pop_item();
        ASSERT_TRUE( received.has_value() );
        ASSERT_TRUE( std::holds_alternative<grab::Event>( *received ) );
        const auto& event = std::get<grab::Event>( *received );
        EXPECT_EQ( event.kind, expected.kind );
        EXPECT_EQ( event.category, expected.category );
        EXPECT_DOUBLE_EQ( event.timestamp, expected.timestamp );
        EXPECT_TRUE( std::holds_alternative<grab::InputKey>( event.payload ) );
        const auto& key = std::get<grab::InputKey>( event.payload );
        EXPECT_EQ( key.code, 42U );
        EXPECT_EQ( key.name, "shift" );

        server->shutdown();
    }

    TEST( ClientErrors,
          DistinguishesConnectionsFromSemanticFailures )
    {
        const grab::Error connection{
            .code        = grab::ErrorCode::EnvironmentChanged,
            .message     = "daemon connection unavailable",
            .capability  = {},
            .target      = {},
            .attempts    = {},
            .disposition = grab::ErrorDisposition::RetrySame,
            .diagnostics = {},
        };
        const grab::Error semantic{
            .code        = grab::ErrorCode::InvalidArgument,
            .message     = "event is invalid",
            .capability  = {},
            .target      = {},
            .attempts    = {},
            .disposition = grab::ErrorDisposition::Fatal,
            .diagnostics = {},
        };

        EXPECT_TRUE( grab::client::is_connection_error( connection ) );
        EXPECT_FALSE( grab::client::is_connection_error( semantic ) );
    }

}    // namespace
