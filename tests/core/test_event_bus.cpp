#include "grab/event.hpp"
#include "grab/event_descriptor.hpp"
#include "grab/result.hpp"
#include "kernel/events/event_bus.hpp"

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
#include <vector>
// clang-format on

namespace
{

    constexpr auto             keyDownKind           = grab::EventKind::KeyDown;
    constexpr auto             keyUpKind             = grab::EventKind::KeyUp;
    constexpr auto             mouseMoveKind         = grab::EventKind::MouseMove;
    constexpr auto             stateSnapshotKind     = grab::EventKind::StateSnapshot;
    constexpr auto             inputCategory         = grab::EventCategory::Input;
    constexpr double           timestamp             = 10.5;
    constexpr std::uint64_t    unsetSequence         = 0U;
    constexpr std::uint64_t    firstSequence         = 1U;
    constexpr std::uint64_t    secondSequence        = 2U;
    constexpr std::uint64_t    thirdSequence         = 3U;
    constexpr std::uint64_t    noOverflows           = 0U;
    constexpr std::uint64_t    oneOverflow           = 1U;
    constexpr std::uint32_t    keyCode               = 42U;
    constexpr std::string_view keyName               = "answer";
    constexpr std::size_t      smallQueueDepth       = 2U;
    constexpr std::size_t      overflowPublishCount  = 4U;
    constexpr std::size_t      firstPublishIndex     = 0U;
    constexpr std::uint64_t    expectedOverflowCount = 2U;
    constexpr int              noNotifications       = 0;
    constexpr int              oneNotification       = 1;
    constexpr double           firstMoveDelta        = 1.0;
    constexpr double           secondMoveDelta       = 2.0;
    constexpr double           lastMoveDelta         = 3.0;
    constexpr std::string_view firstMoveAxis         = "first";
    constexpr std::string_view secondMoveAxis        = "second";
    constexpr std::string_view lastMoveAxis          = "last";
    constexpr std::string_view firstSnapshotJson     = R"({"state":"first"})";
    constexpr std::string_view secondSnapshotJson    = R"({"state":"second"})";
    constexpr std::string_view liveSnapshotJson      = R"({"state":"live"})";
    constexpr auto             publishTimeout        = std::chrono::seconds{ 2 };
    constexpr auto             notifyTimeout         = std::chrono::seconds{ 2 };

    [[nodiscard]]
    grab::Event
    make_key_event( grab::EventKind kind )
    {
        return grab::Event{
            .timestamp = timestamp,
            .sequence  = unsetSequence,
            .kind      = kind,
            .category  = grab::category_of( kind ),
            .payload   = grab::Payload{ grab::InputKey{
                .code = keyCode,
                .name = std::string{ keyName },
            } },
        };
    }

    [[nodiscard]]
    grab::Event
    make_mouse_move_event( std::string_view axis,
                           double           delta )
    {
        return grab::Event{
            .timestamp = timestamp,
            .sequence  = unsetSequence,
            .kind      = mouseMoveKind,
            .category  = grab::category_of( mouseMoveKind ),
            .payload   = grab::Payload{ grab::MouseMove{
                .axis  = std::string{ axis },
                .delta = delta,
            } },
        };
    }

    [[nodiscard]]
    grab::Event
    make_state_snapshot_event( std::string_view json )
    {
        return grab::Event{
            .timestamp = timestamp,
            .sequence  = unsetSequence,
            .kind      = stateSnapshotKind,
            .category  = grab::category_of( stateSnapshotKind ),
            .payload   = grab::Payload{ grab::StateSnapshot{
                .json = std::string{ json },
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

    void
    expect_state_snapshot( const std::optional<grab::Event>& event,
                           std::string_view                  json,
                           std::uint64_t                     sequence )
    {
        ASSERT_TRUE( event.has_value() );
        EXPECT_EQ( event->sequence, sequence );
        EXPECT_EQ( event->kind, stateSnapshotKind );
        const auto* payload = std::get_if<grab::StateSnapshot>( &event->payload );
        ASSERT_NE( payload, nullptr );
        EXPECT_EQ( payload->json, json );
    }

    void
    expect_event_item( const std::optional<grab::SubscriptionEvent>& item,
                       grab::EventKind                               kind,
                       std::uint64_t                                 sequence )
    {
        ASSERT_TRUE( item.has_value() );
        const auto* event = std::get_if<grab::Event>( &*item );
        ASSERT_NE( event, nullptr );
        EXPECT_EQ( event->kind, kind );
        EXPECT_EQ( event->sequence, sequence );
    }

    void
    expect_queue_gap( const std::optional<grab::SubscriptionEvent>& item,
                      std::uint64_t last_delivered_sequence )
    {
        ASSERT_TRUE( item.has_value() );
        const auto* gap = std::get_if<grab::QueueGapMarker>( &*item );
        ASSERT_NE( gap, nullptr );
        EXPECT_EQ( gap->code, grab::ErrorCode::QueueGap );
        EXPECT_EQ( gap->last_delivered_sequence, last_delivered_sequence );
    }

    void
    expect_demand_transition( const std::vector<grab::EventKind>& kinds,
                              const std::vector<bool>&            states,
                              std::size_t                         expected_count,
                              bool                                expected_state )
    {
        ASSERT_EQ( kinds.size(), expected_count );
        ASSERT_EQ( states.size(), expected_count );
        ASSERT_FALSE( kinds.empty() );
        EXPECT_EQ( kinds.back(), keyDownKind );
        EXPECT_EQ( states.back(), expected_state );
    }

}    // namespace

TEST( QueueOptions,
      DefaultsAreVisibleAndOverridable )
{
    constexpr std::size_t        customCapacity = 2U;

    constexpr grab::QueueOptions defaults;
    EXPECT_EQ( defaults.capacity, grab::QueueOptions::defaultCapacity );
    EXPECT_EQ( defaults.capacity, 1'024U );
    EXPECT_EQ( defaults.overflow, grab::QueueOptions::defaultOverflow );
    EXPECT_EQ( defaults.overflow, grab::QueueOverflowPolicy::Coalesce );

    constexpr grab::QueueOptions customized{
        .capacity = customCapacity,
        .overflow = grab::QueueOverflowPolicy::NeverDrop,
    };
    EXPECT_EQ( customized.capacity, customCapacity );
    EXPECT_EQ( customized.overflow, grab::QueueOverflowPolicy::NeverDrop );
}

TEST( EventBus,
      PublishDeliversToMatchingSubscriberWithSequence )
{
    grab::EventBus bus;
    auto           matching     = bus.subscribe( grab::EventFilter{
        .kinds      = { keyDownKind },
        .categories = {},
    } );
    auto           non_matching = bus.subscribe( grab::EventFilter{
        .kinds      = { mouseMoveKind },
        .categories = {},
    } );

    bus.publish( make_key_event( keyDownKind ) );

    auto delivered = matching.try_pop();
    ASSERT_TRUE( delivered.has_value() );
    EXPECT_EQ( delivered->sequence, firstSequence );
    EXPECT_EQ( delivered->kind, keyDownKind );
    EXPECT_EQ( delivered->category, inputCategory );
    EXPECT_FALSE( non_matching.try_pop().has_value() );
}

TEST( EventBus,
      SequenceIsMonotonic )
{
    grab::EventBus bus;
    auto           subscription = bus.subscribe( grab::EventFilter{} );

    bus.publish( make_key_event( keyDownKind ) );
    bus.publish( make_key_event( keyUpKind ) );
    bus.publish( make_mouse_move_event( firstMoveAxis, firstMoveDelta ) );

    const auto first = subscription.try_pop();
    ASSERT_TRUE( first.has_value() );
    EXPECT_EQ( first->sequence, firstSequence );

    const auto second = subscription.try_pop();
    ASSERT_TRUE( second.has_value() );
    EXPECT_EQ( second->sequence, secondSequence );

    const auto third = subscription.try_pop();
    ASSERT_TRUE( third.has_value() );
    EXPECT_EQ( third->sequence, thirdSequence );

    EXPECT_FALSE( subscription.try_pop().has_value() );
}

TEST( EventBus,
      SubscribeAssignsStableNonNilSubscriptionId )
{
    grab::EventBus bus;
    auto           subscription = bus.subscribe( grab::EventFilter{} );
    const auto     id           = subscription.id();

    EXPECT_FALSE( id.value.is_nil() );
    EXPECT_EQ( subscription.id(), id );
}

TEST( EventBus,
      CurrentSetReplayPrecedesLivePublish )
{
    static_assert( grab::replay_policy_of( stateSnapshotKind ) ==
                   grab::ReplayPolicy::CurrentSet );

    grab::EventBus bus;
    bus.register_snapshot_provider(
        stateSnapshotKind,
        []
        {
            return std::vector<grab::Event>{
                make_state_snapshot_event( firstSnapshotJson ),
                make_state_snapshot_event( secondSnapshotJson ),
            };
        }
    );

    auto subscription = bus.subscribe( grab::EventFilter{
        .kinds      = { stateSnapshotKind },
        .categories = {},
    } );
    bus.publish( make_state_snapshot_event( liveSnapshotJson ) );

    expect_state_snapshot( subscription.try_pop(), firstSnapshotJson, firstSequence );
    expect_state_snapshot( subscription.try_pop(), secondSnapshotJson, secondSequence );
    expect_state_snapshot( subscription.try_pop(), liveSnapshotJson, thirdSequence );
    EXPECT_FALSE( subscription.try_pop().has_value() );
}

TEST( EventBus,
      MotionCoalescesUnderPressure )
{
    grab::EventBus bus;
    auto           subscription = bus.subscribe(
        grab::EventFilter{
            .kinds      = { mouseMoveKind },
            .categories = {},
        },
        grab::QueueOptions{ .capacity = smallQueueDepth }
    );

    auto publish_future = std::async(
        std::launch::async,
        [&]
        {
            bus.publish( make_mouse_move_event( firstMoveAxis, firstMoveDelta ) );
            bus.publish( make_mouse_move_event( secondMoveAxis, secondMoveDelta ) );
            bus.publish( make_mouse_move_event( lastMoveAxis, lastMoveDelta ) );
        }
    );
    ASSERT_EQ( publish_future.wait_for( publishTimeout ), std::future_status::ready );
    publish_future.get();

    expect_mouse_move( subscription.try_pop(), firstMoveAxis, firstMoveDelta );
    expect_mouse_move( subscription.try_pop(), lastMoveAxis, lastMoveDelta );

    EXPECT_FALSE( subscription.try_pop().has_value() );
    EXPECT_EQ( subscription.overflow_count(), noOverflows );
    EXPECT_FALSE( subscription.lagging() );
}

TEST( EventBus,
      NeverDropSurfacesMotionOverflowWithoutCoalescing )
{
    grab::EventBus bus;
    auto           subscription = bus.subscribe(
        grab::EventFilter{
            .kinds      = { mouseMoveKind },
            .categories = {},
        },
        grab::QueueOptions{
            .capacity = smallQueueDepth,
            .overflow = grab::QueueOverflowPolicy::NeverDrop,
        }
    );

    bus.publish( make_mouse_move_event( firstMoveAxis, firstMoveDelta ) );
    bus.publish( make_mouse_move_event( secondMoveAxis, secondMoveDelta ) );
    bus.publish( make_mouse_move_event( lastMoveAxis, lastMoveDelta ) );

    const auto first = subscription.try_pop();
    ASSERT_TRUE( first.has_value() );
    expect_mouse_move( first, firstMoveAxis, firstMoveDelta );
    EXPECT_EQ( first->sequence, firstSequence );

    // P1.6b replaces the newest queued event with an explicit resync gap.
    expect_queue_gap( subscription.try_pop_item(), unsetSequence );

    EXPECT_FALSE( subscription.try_pop_item().has_value() );
    EXPECT_EQ( subscription.overflow_count(), oneOverflow );
    EXPECT_EQ( subscription.dropped_count(), oneOverflow );
    EXPECT_TRUE( subscription.lagging() );
    EXPECT_TRUE( subscription.needs_resync() );
}

TEST( EventBus,
      EdgeOverflowMarksLaggingAndCounts )
{
    grab::EventBus bus;
    auto           subscription = bus.subscribe(
        grab::EventFilter{
            .kinds      = { keyDownKind },
            .categories = {},
        },
        smallQueueDepth
    );

    auto publish_future =
        std::async( std::launch::async,
                    [&]
                    {
                        for( std::size_t index = firstPublishIndex;
                             index < overflowPublishCount;
                             ++index )
                        {
                            bus.publish( make_key_event( keyDownKind ) );
                        }
                    } );
    ASSERT_EQ( publish_future.wait_for( publishTimeout ), std::future_status::ready );
    publish_future.get();

    EXPECT_EQ( subscription.overflow_count(), expectedOverflowCount );
    EXPECT_EQ( subscription.dropped_count(), expectedOverflowCount );
    EXPECT_TRUE( subscription.lagging() );
    EXPECT_TRUE( subscription.needs_resync() );

    expect_event_item( subscription.try_pop_item(), keyDownKind, firstSequence );

    // P1.6b keeps edge drop accounting but exposes loss as a resync gap.
    expect_queue_gap( subscription.try_pop_item(), unsetSequence );

    EXPECT_FALSE( subscription.try_pop_item().has_value() );
}

TEST( EventBus,
      NeverDropOverflowRequiresResyncAndReportsLastDeliveredSequence )
{
    grab::EventBus bus;
    auto           subscription = bus.subscribe(
        grab::EventFilter{
            .kinds      = { mouseMoveKind },
            .categories = {},
        },
        grab::QueueOptions{
            .capacity = 1U,
            .overflow = grab::QueueOverflowPolicy::NeverDrop,
        }
    );

    bus.publish( make_mouse_move_event( firstMoveAxis, firstMoveDelta ) );
    const auto delivered = subscription.try_pop();
    ASSERT_TRUE( delivered.has_value() );
    EXPECT_EQ( delivered->sequence, firstSequence );

    bus.publish( make_mouse_move_event( secondMoveAxis, secondMoveDelta ) );
    bus.publish( make_mouse_move_event( lastMoveAxis, lastMoveDelta ) );

    EXPECT_EQ( subscription.overflow_count(), oneOverflow );
    EXPECT_EQ( subscription.dropped_count(), oneOverflow );
    EXPECT_TRUE( subscription.lagging() );
    EXPECT_TRUE( subscription.needs_resync() );

    expect_queue_gap( subscription.try_pop_item(), firstSequence );
    EXPECT_FALSE( subscription.try_pop_item().has_value() );
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

    bus.publish( make_key_event( keyDownKind ) );

    EXPECT_EQ( notified_future.wait_for( notifyTimeout ), std::future_status::ready );
}

TEST( EventBus,
      SubscriptionRaiiUnsubscribes )
{
    grab::EventBus   bus;
    auto             survivor = bus.subscribe( grab::EventFilter{} );
    std::atomic<int> destroyed_notifications{ noNotifications };

    {
        auto destroyed = bus.subscribe( grab::EventFilter{} );
        destroyed.set_notify(
            [&]
            {
                destroyed_notifications.fetch_add( oneNotification,
                                                   std::memory_order_relaxed );
            }
        );
    }

    bus.publish( make_key_event( keyDownKind ) );

    auto delivered = survivor.try_pop();
    ASSERT_TRUE( delivered.has_value() );
    EXPECT_EQ( delivered->sequence, firstSequence );
    EXPECT_EQ( destroyed_notifications.load( std::memory_order_relaxed ),
               noNotifications );
}

TEST( EventBus,
      DemandCallbackFiresOnlyAtFirstAndLastSubscriber )
{
    grab::EventBus               bus;
    std::vector<grab::EventKind> transition_kinds;
    std::vector<bool>            transition_states;
    bus.set_demand_callback(
        [&]( grab::EventKind kind, bool enabled )
        {
            transition_kinds.push_back( kind );
            transition_states.push_back( enabled );
        }
    );

    EXPECT_EQ( bus.subscription_refcount( keyDownKind ), 0U );
    {
        auto first = bus.subscribe( grab::EventFilter{
            .kinds      = { keyDownKind },
            .categories = {},
        } );
        EXPECT_EQ( bus.subscription_refcount( keyDownKind ), 1U );
        expect_demand_transition( transition_kinds, transition_states, 1U, true );

        {
            auto second = bus.subscribe( grab::EventFilter{
                .kinds      = { keyDownKind },
                .categories = {},
            } );
            EXPECT_EQ( bus.subscription_refcount( keyDownKind ), 2U );
            EXPECT_EQ( transition_states.size(), 1U );
        }

        EXPECT_EQ( bus.subscription_refcount( keyDownKind ), 1U );
        EXPECT_EQ( transition_states.size(), 1U );
    }

    EXPECT_EQ( bus.subscription_refcount( keyDownKind ), 0U );
    expect_demand_transition( transition_kinds, transition_states, 2U, false );
}
