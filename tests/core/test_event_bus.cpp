#include "grab/event.hpp"
#include "grab/event_bus.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
// clang-format on

namespace
{

    constexpr auto             kKeyDownKind           = grab::EventKind::key_down;
    constexpr auto             kKeyUpKind             = grab::EventKind::key_up;
    constexpr auto             kMouseMoveKind         = grab::EventKind::mouse_move;
    constexpr auto             kInputCategory         = grab::EventCategory::input;
    constexpr double           kTimestamp             = 10.5;
    constexpr std::uint64_t    kUnsetSequence         = 0U;
    constexpr std::uint64_t    kFirstSequence         = 1U;
    constexpr std::uint64_t    kSecondSequence        = 2U;
    constexpr std::uint64_t    kThirdSequence         = 3U;
    constexpr std::uint64_t    kNoOverflows           = 0U;
    constexpr std::uint32_t    kKeyCode               = 42U;
    constexpr std::string_view kKeyName               = "answer";
    constexpr std::size_t      kSmallQueueDepth       = 2U;
    constexpr std::size_t      kOverflowPublishCount  = 4U;
    constexpr std::size_t      kFirstPublishIndex     = 0U;
    constexpr std::uint64_t    kExpectedOverflowCount = 2U;
    constexpr int              kNoNotifications       = 0;
    constexpr int              kOneNotification       = 1;
    constexpr double           kFirstMoveDelta        = 1.0;
    constexpr double           kSecondMoveDelta       = 2.0;
    constexpr double           kLastMoveDelta         = 3.0;
    constexpr std::string_view kFirstMoveAxis         = "first";
    constexpr std::string_view kSecondMoveAxis        = "second";
    constexpr std::string_view kLastMoveAxis          = "last";
    constexpr auto             kPublishTimeout        = std::chrono::seconds{ 2 };
    constexpr auto             kNotifyTimeout         = std::chrono::seconds{ 2 };

    [[nodiscard]]
    grab::Event
    make_key_event( grab::EventKind kind )
    {
        return grab::Event{
            .timestamp = kTimestamp,
            .sequence  = kUnsetSequence,
            .kind      = kind,
            .category  = grab::category_of( kind ),
            .payload   = grab::Payload{ grab::InputKey{
                .code = kKeyCode,
                .name = std::string{ kKeyName },
            } },
        };
    }

    [[nodiscard]]
    grab::Event
    make_mouse_move_event( std::string_view axis,
                           double           delta )
    {
        return grab::Event{
            .timestamp = kTimestamp,
            .sequence  = kUnsetSequence,
            .kind      = kMouseMoveKind,
            .category  = grab::category_of( kMouseMoveKind ),
            .payload   = grab::Payload{ grab::MouseMove{
                .axis  = std::string{ axis },
                .delta = delta,
            } },
        };
    }

    void
    expect_mouse_move( const std::optional<grab::Event>& event,
                       std::string_view                  axis,
                       double                            delta )
    {
        ASSERT_TRUE( event.has_value() );
        const auto* payload = std::get_if<grab::MouseMove>( &event->payload );
        ASSERT_NE( payload, nullptr );
        EXPECT_EQ( payload->axis, axis );
        EXPECT_DOUBLE_EQ( payload->delta, delta );
    }

}    // namespace

TEST( EventBus,
      PublishDeliversToMatchingSubscriberWithSequence )
{
    grab::EventBus bus;
    auto           matching     = bus.subscribe( grab::EventFilter{
        .kinds      = { kKeyDownKind },
        .categories = {},
    } );
    auto           non_matching = bus.subscribe( grab::EventFilter{
        .kinds      = { kMouseMoveKind },
        .categories = {},
    } );

    bus.publish( make_key_event( kKeyDownKind ) );

    auto delivered = matching.try_pop();
    ASSERT_TRUE( delivered.has_value() );
    EXPECT_EQ( delivered->sequence, kFirstSequence );
    EXPECT_EQ( delivered->kind, kKeyDownKind );
    EXPECT_EQ( delivered->category, kInputCategory );
    EXPECT_FALSE( non_matching.try_pop().has_value() );
}

TEST( EventBus,
      SequenceIsMonotonic )
{
    grab::EventBus bus;
    auto           subscription = bus.subscribe( grab::EventFilter{} );

    bus.publish( make_key_event( kKeyDownKind ) );
    bus.publish( make_key_event( kKeyUpKind ) );
    bus.publish( make_mouse_move_event( kFirstMoveAxis, kFirstMoveDelta ) );

    const auto first = subscription.try_pop();
    ASSERT_TRUE( first.has_value() );
    EXPECT_EQ( first->sequence, kFirstSequence );

    const auto second = subscription.try_pop();
    ASSERT_TRUE( second.has_value() );
    EXPECT_EQ( second->sequence, kSecondSequence );

    const auto third = subscription.try_pop();
    ASSERT_TRUE( third.has_value() );
    EXPECT_EQ( third->sequence, kThirdSequence );

    EXPECT_FALSE( subscription.try_pop().has_value() );
}

TEST( EventBus,
      MotionCoalescesUnderPressure )
{
    grab::EventBus bus;
    auto           subscription = bus.subscribe(
        grab::EventFilter{
            .kinds      = { kMouseMoveKind },
            .categories = {},
        },
        kSmallQueueDepth
    );

    auto publish_future = std::async(
        std::launch::async,
        [&]
        {
            bus.publish( make_mouse_move_event( kFirstMoveAxis, kFirstMoveDelta ) );
            bus.publish( make_mouse_move_event( kSecondMoveAxis, kSecondMoveDelta ) );
            bus.publish( make_mouse_move_event( kLastMoveAxis, kLastMoveDelta ) );
        }
    );
    ASSERT_EQ( publish_future.wait_for( kPublishTimeout ), std::future_status::ready );
    publish_future.get();

    expect_mouse_move( subscription.try_pop(), kFirstMoveAxis, kFirstMoveDelta );
    expect_mouse_move( subscription.try_pop(), kLastMoveAxis, kLastMoveDelta );

    EXPECT_FALSE( subscription.try_pop().has_value() );
    EXPECT_EQ( subscription.overflow_count(), kNoOverflows );
    EXPECT_FALSE( subscription.lagging() );
}

TEST( EventBus,
      EdgeOverflowMarksLaggingAndCounts )
{
    grab::EventBus bus;
    auto           subscription = bus.subscribe(
        grab::EventFilter{
            .kinds      = { kKeyDownKind },
            .categories = {},
        },
        kSmallQueueDepth
    );

    auto publish_future =
        std::async( std::launch::async,
                    [&]
                    {
                        for( std::size_t index = kFirstPublishIndex;
                             index < kOverflowPublishCount;
                             ++index )
                        {
                            bus.publish( make_key_event( kKeyDownKind ) );
                        }
                    } );
    ASSERT_EQ( publish_future.wait_for( kPublishTimeout ), std::future_status::ready );
    publish_future.get();

    EXPECT_EQ( subscription.overflow_count(), kExpectedOverflowCount );
    EXPECT_TRUE( subscription.lagging() );

    EXPECT_TRUE( subscription.try_pop().has_value() );
    EXPECT_TRUE( subscription.try_pop().has_value() );
    EXPECT_FALSE( subscription.try_pop().has_value() );
}

TEST( EventBus,
      NotifyFiresOnEnqueue )
{
    grab::EventBus     bus;
    auto               subscription = bus.subscribe( grab::EventFilter{} );
    std::promise<void> notified;
    auto               notified_future = notified.get_future();

    subscription.set_notify(
        [&]
        {
            auto nested = bus.subscribe( grab::EventFilter{} );
            EXPECT_FALSE( nested.try_pop().has_value() );
            notified.set_value();
        }
    );

    bus.publish( make_key_event( kKeyDownKind ) );

    EXPECT_EQ( notified_future.wait_for( kNotifyTimeout ), std::future_status::ready );
}

TEST( EventBus,
      SubscriptionRaiiUnsubscribes )
{
    grab::EventBus   bus;
    auto             survivor = bus.subscribe( grab::EventFilter{} );
    std::atomic<int> destroyed_notifications{ kNoNotifications };

    {
        auto destroyed = bus.subscribe( grab::EventFilter{} );
        destroyed.set_notify(
            [&]
            {
                destroyed_notifications.fetch_add( kOneNotification,
                                                   std::memory_order_relaxed );
            }
        );
    }

    bus.publish( make_key_event( kKeyDownKind ) );

    auto delivered = survivor.try_pop();
    ASSERT_TRUE( delivered.has_value() );
    EXPECT_EQ( delivered->sequence, kFirstSequence );
    EXPECT_EQ( destroyed_notifications.load( std::memory_order_relaxed ),
               kNoNotifications );
}
