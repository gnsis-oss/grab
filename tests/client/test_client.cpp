#include "client/client.hpp"
#include "client/loopback_transport.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/event_descriptor.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <memory>
#include <utility>
#include <variant>
// clang-format on

namespace
{

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

}    // namespace
