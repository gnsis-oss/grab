#include "drivers/desktop/x11/x11_event_source.hpp"
#include "drivers/desktop/x11/x11_routes.hpp"
#include "drivers/desktop/x11/x11_runtime.hpp"
#include "drivers/desktop/x11/x11_topology_source.hpp"
#include "drivers/desktop/x11/x11_xtest_seat.hpp"
#include "grab/context.hpp"
#include "grab/event.hpp"
#include "grab/origin.hpp"
#include "grab/session.hpp"
#include "grab/space.hpp"
#include "kernel/events/event_bus.hpp"
#include "kernel/lifecycle/session_impl.hpp"
#include "spi/event_source.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
// clang-format on

namespace
{

    constexpr std::uint8_t     test_keycode            = 38U;
    constexpr std::uint8_t     test_shift_keycode      = 50U;
    constexpr std::uint8_t     unmapped_keycode        = 93U;
    constexpr std::uint8_t     test_button             = 1U;
    constexpr std::int16_t     test_pointer_x          = 64;
    constexpr std::int16_t     test_pointer_y          = 72;
    constexpr std::size_t      no_subscriptions        = 0U;
    constexpr std::size_t      one_subscription        = 1U;
    constexpr std::size_t      maximum_pump_iterations = 10U;
    constexpr std::uint64_t    initial_generation      = 0U;
    constexpr std::size_t      no_refreshes            = 0U;
    constexpr double           timestampSlackSeconds   = 60.0;
    constexpr std::string_view mouseButtonDownWireName{ "input.mouse_button_down" };
    constexpr std::string_view mouseButtonUpWireName{ "input.mouse_button_up" };
    constexpr std::string_view mouseClickWireName{ "input.mouse_click" };
    constexpr std::string_view keyDownWireName{ "input.key_down" };
    constexpr std::string_view keyUpWireName{ "input.key_up" };
    constexpr std::string_view expectedTestKeyName{ "a" };
    constexpr std::string_view emptyKeyName{};
    constexpr std::string_view emptyButtonName{};
    constexpr auto             event_wait_budget    = std::chrono::seconds{ 5 };
    constexpr auto             short_context_budget = std::chrono::milliseconds{ 400 };
    constexpr auto             short_wait_budget    = std::chrono::milliseconds{ 250 };

    class X11EventSourceKeyboardTest : public testing::Test
    {
        protected:

            void
            SetUp() override
            {
                const char* const display = std::getenv( "DISPLAY" );
                if( display == nullptr || std::string_view{ display }.empty() )
                {
                    GTEST_SKIP() << "requires Xvfb (DISPLAY is not set)";
                }

                const grab::OperationContext start_context{
                    .deadline = grab::Deadline::unbounded(),
                };
                const auto started = runtime_.start( start_context );
                ASSERT_TRUE( started.has_value() );
                started_ = true;

                runtime_.set_event_sink(
                    [this]( grab::Event&& event )
                    {
                        events_.push_back( std::move( event ) );
                    }
                );
                event_source_ = runtime_.event_source();
                ASSERT_NE( event_source_, nullptr );
                seat_ = runtime_.native_seat();
                ASSERT_NE( seat_, nullptr );
            }

            void
            TearDown() override
            {
                if( started_ )
                {
                    EXPECT_TRUE( runtime_.stop().has_value() );
                }
            }

            [[nodiscard]]
            const grab::Event*
            find_key_event( grab::EventKind kind,
                            std::uint8_t    keycode ) const
            {
                const auto found = std::ranges::find_if(
                    events_,
                    [kind, keycode]( const grab::Event& event )
                    {
                        return event.kind ==
                               kind &&
                               std::get<grab::InputKey>( event.payload ).code == keycode;
                    }
                );
                return found == events_.end() ? nullptr : &*found;
            }

            [[nodiscard]]
            grab::Result<void>
            wait_for( const grab::spi::EventSpec& spec )
            {
                const grab::OperationContext wait_context{
                    .deadline = grab::Deadline::after( event_wait_budget ),
                };
                return event_source_->wait_for_event( spec,
                                                      wait_context,
                                                      event_wait_budget );
            }

            grab::drivers::desktop::x11::X11Runtime    runtime_;
            std::vector<grab::Event>                   events_;
            grab::spi::EventSource*                    event_source_{};
            grab::drivers::desktop::x11::X11InputSeat* seat_{};
            bool                                       started_{};
    };

    class X11EventSourceShiftedKeyboardTest : public X11EventSourceKeyboardTest
    {
        protected:

            void
            SetUp() override
            {
                const char* const display = std::getenv( "DISPLAY" );
                if( display == nullptr || std::string_view{ display }.empty() )
                {
                    GTEST_SKIP() << "requires Xvfb (DISPLAY is not set)";
                }

                auto opened_shift_seat = grab::input::Seat::open();
                ASSERT_TRUE( opened_shift_seat.has_value() );
                shift_seat_.emplace( std::move( *opened_shift_seat ) );

                const auto pressed = shift_seat_->key( test_shift_keycode, true );
                shift_held_        = pressed.has_value();
                const auto flushed = shift_seat_->flush();
                ASSERT_TRUE( pressed.has_value() );
                ASSERT_TRUE( flushed.has_value() );

                // Build the event source's XKB state while Shift is depressed. A
                // modifier-sensitive lookup would now report "A", not the base "a".
                X11EventSourceKeyboardTest::SetUp();
            }

            void
            TearDown() override
            {
                if( shift_held_ )
                {
                    const auto released = shift_seat_->key( test_shift_keycode, false );
                    const auto flushed  = shift_seat_->flush();
                    shift_held_         = false;
                    EXPECT_TRUE( released.has_value() );
                    EXPECT_TRUE( flushed.has_value() );
                }
                X11EventSourceKeyboardTest::TearDown();
            }

        private:

            std::optional<grab::input::Seat> shift_seat_;
            bool                             shift_held_{};
    };

}    // namespace

// NOLINTBEGIN(readability-trailing-comma)

// GoogleTest assertion macros inflate the reported cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_F( X11EventSourceKeyboardTest,
        SyntheticKeyPressCarriesBaseNameAndRawCode )
{
    const grab::spi::EventSpec spec{ std::string{ keyDownWireName } };
    ASSERT_TRUE( event_source_->enable( spec ).has_value() );

    ASSERT_TRUE( seat_->key( test_keycode, true ).has_value() );
    ASSERT_TRUE( seat_->key( test_keycode, false ).has_value() );
    ASSERT_TRUE( seat_->flush().has_value() );
    ASSERT_TRUE( wait_for( spec ).has_value() );

    const auto* const injected_key_down =
        find_key_event( grab::EventKind::KeyDown, test_keycode );
    ASSERT_NE( injected_key_down, nullptr );
    const auto& key = std::get<grab::InputKey>( injected_key_down->payload );
    EXPECT_EQ( key.code, test_keycode );
    EXPECT_EQ( key.name, expectedTestKeyName );
    EXPECT_EQ( injected_key_down->origin, grab::EventOrigin::InjectedSelf );
    // Timestamps are wall-clock epoch seconds (spec: uniform event clock),
    // not the X server's millisecond counter.
    const double now_s = std::chrono::duration<double>(
                             std::chrono::system_clock::now().time_since_epoch()
    )
                             .count();
    EXPECT_GT( injected_key_down->timestamp, now_s - timestampSlackSeconds );
    EXPECT_LT( injected_key_down->timestamp, now_s + timestampSlackSeconds );
    EXPECT_TRUE( std::ranges::none_of( events_,
                                       []( const grab::Event& event )
                                       {
                                           return event.origin ==
                                                  grab::EventOrigin::Unknown;
                                       } ) );
}

TEST_F( X11EventSourceKeyboardTest,
        SyntheticKeyReleaseCarriesSameNameAsPress )
{
    const grab::spi::EventSpec down_spec{ std::string{ keyDownWireName } };
    const grab::spi::EventSpec up_spec{ std::string{ keyUpWireName } };
    ASSERT_TRUE( event_source_->enable( down_spec ).has_value() );
    ASSERT_TRUE( event_source_->enable( up_spec ).has_value() );

    const auto pressed  = seat_->key( test_keycode, true );
    const auto released = seat_->key( test_keycode, false );
    const auto flushed  = seat_->flush();
    ASSERT_TRUE( pressed.has_value() );
    ASSERT_TRUE( released.has_value() );
    ASSERT_TRUE( flushed.has_value() );
    ASSERT_TRUE( wait_for( up_spec ).has_value() );

    const auto* const key_down =
        find_key_event( grab::EventKind::KeyDown, test_keycode );
    const auto* const key_up = find_key_event( grab::EventKind::KeyUp, test_keycode );
    ASSERT_NE( key_down, nullptr );
    ASSERT_NE( key_up, nullptr );

    const auto& down_payload = std::get<grab::InputKey>( key_down->payload );
    const auto& up_payload   = std::get<grab::InputKey>( key_up->payload );
    EXPECT_EQ( down_payload.code, test_keycode );
    EXPECT_EQ( up_payload.code, test_keycode );
    EXPECT_EQ( down_payload.name, expectedTestKeyName );
    EXPECT_EQ( up_payload.name, down_payload.name );
}

TEST_F( X11EventSourceKeyboardTest,
        UnmappedKeycodeStillDeliversWithEmptyName )
{
    const grab::spi::EventSpec spec{ std::string{ keyDownWireName } };
    ASSERT_TRUE( event_source_->enable( spec ).has_value() );

    const auto pressed  = seat_->key( unmapped_keycode, true );
    const auto released = seat_->key( unmapped_keycode, false );
    const auto flushed  = seat_->flush();
    ASSERT_TRUE( pressed.has_value() );
    ASSERT_TRUE( released.has_value() );
    ASSERT_TRUE( flushed.has_value() );
    ASSERT_TRUE( wait_for( spec ).has_value() );

    const auto* const key_down =
        find_key_event( grab::EventKind::KeyDown, unmapped_keycode );
    ASSERT_NE( key_down, nullptr );
    const auto& payload = std::get<grab::InputKey>( key_down->payload );
    EXPECT_EQ( payload.code, unmapped_keycode );
    EXPECT_EQ( payload.name, emptyKeyName );
}

TEST_F( X11EventSourceShiftedKeyboardTest,
        ShiftHeldKeyPressStillCarriesBaseName )
{
    const grab::spi::EventSpec spec{ std::string{ keyDownWireName } };
    ASSERT_TRUE( event_source_->enable( spec ).has_value() );

    const auto key_pressed  = seat_->key( test_keycode, true );
    const auto key_released = seat_->key( test_keycode, false );
    const auto flushed      = seat_->flush();
    ASSERT_TRUE( key_pressed.has_value() );
    ASSERT_TRUE( key_released.has_value() );
    ASSERT_TRUE( flushed.has_value() );
    ASSERT_TRUE( wait_for( spec ).has_value() );

    const auto* const key_down =
        find_key_event( grab::EventKind::KeyDown, test_keycode );
    ASSERT_NE( key_down, nullptr );
    const auto& payload = std::get<grab::InputKey>( key_down->payload );
    EXPECT_EQ( payload.code, test_keycode );
    EXPECT_EQ( payload.name, expectedTestKeyName );
}

// GoogleTest assertion macros inflate the reported cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST( X11EventSource,
      MotionBatchCarriesQueriedPositionInGlobalSpace )
{
    const char* const display = std::getenv( "DISPLAY" );
    if( display == nullptr || std::string_view{ display }.empty() )
    {
        GTEST_SKIP() << "requires Xvfb (DISPLAY is not set)";
    }

    grab::drivers::desktop::x11::X11Runtime runtime;
    const grab::OperationContext            start_context{
        .deadline = grab::Deadline::unbounded(),
    };
    ASSERT_TRUE( runtime.start( start_context ).has_value() );
    const auto* const capture_route = runtime.capture_route();
    ASSERT_NE( capture_route, nullptr );

    std::vector<grab::Event> events;
    runtime.set_event_sink(
        [&events]( grab::Event&& event )
        {
            events.push_back( std::move( event ) );
        }
    );

    auto* const event_source = runtime.event_source();
    ASSERT_NE( event_source, nullptr );
    const grab::spi::EventSpec spec{ "input.mouse_move" };
    ASSERT_TRUE( event_source->enable( spec ).has_value() );

    auto* const seat = runtime.native_seat();
    ASSERT_NE( seat, nullptr );
    ASSERT_TRUE(
        seat->move_pointer_absolute( test_pointer_x, test_pointer_y ).has_value()
    );
    ASSERT_TRUE( seat->flush().has_value() );

    const grab::OperationContext wait_context{
        .deadline = grab::Deadline::after( event_wait_budget ),
    };
    ASSERT_TRUE(
        event_source->wait_for_event( spec, wait_context, event_wait_budget ).has_value()
    );

    bool saw_motion{};
    for( const auto& event : events )
    {
        if( event.kind != grab::EventKind::MouseMove )
        {
            continue;
        }
        saw_motion          = true;
        const auto& payload = std::get<grab::MouseMove>( event.payload );
        if( !payload.position.has_value() )
        {
            ADD_FAILURE() << "motion batch was not position-stamped";
            continue;
        }
        const auto& position = *payload.position;
        EXPECT_DOUBLE_EQ( position.x, static_cast<double>( test_pointer_x ) );
        EXPECT_DOUBLE_EQ( position.y, static_cast<double>( test_pointer_y ) );
        EXPECT_EQ( position.space, capture_route->global_space() );
    }
    EXPECT_TRUE( saw_motion );

    ASSERT_TRUE( runtime.stop().has_value() );
}

// GoogleTest assertion macros inflate the reported cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST( X11EventSource,
      SyntheticButtonPressAndReleaseYieldDownAndUp )
{
    const char* const display = std::getenv( "DISPLAY" );
    if( display == nullptr || std::string_view{ display }.empty() )
    {
        GTEST_SKIP() << "requires Xvfb (DISPLAY is not set)";
    }

    grab::drivers::desktop::x11::X11Runtime runtime;
    const grab::OperationContext            start_context{
        .deadline = grab::Deadline::unbounded(),
    };
    ASSERT_TRUE( runtime.start( start_context ).has_value() );

    std::vector<grab::Event> events;
    runtime.set_event_sink(
        [&events]( grab::Event&& event )
        {
            events.push_back( std::move( event ) );
        }
    );

    auto* const event_source = runtime.event_source();
    ASSERT_NE( event_source, nullptr );
    const grab::spi::EventSpec down_spec{ std::string{ mouseButtonDownWireName } };
    const grab::spi::EventSpec up_spec{ std::string{ mouseButtonUpWireName } };
    ASSERT_TRUE( event_source->enable( down_spec ).has_value() );
    ASSERT_TRUE( event_source->enable( up_spec ).has_value() );

    auto* const seat = runtime.native_seat();
    ASSERT_NE( seat, nullptr );
    ASSERT_TRUE( seat->button( test_button, true ).has_value() );
    ASSERT_TRUE( seat->flush().has_value() );

    const grab::OperationContext down_wait_context{
        .deadline = grab::Deadline::after( event_wait_budget ),
    };
    ASSERT_TRUE( event_source
                     ->wait_for_event( down_spec, down_wait_context, event_wait_budget )
                     .has_value() );

    ASSERT_TRUE( seat->button( test_button, false ).has_value() );
    ASSERT_TRUE( seat->flush().has_value() );

    const grab::OperationContext up_wait_context{
        .deadline = grab::Deadline::after( event_wait_budget ),
    };
    ASSERT_TRUE( event_source
                     ->wait_for_event( up_spec, up_wait_context, event_wait_budget )
                     .has_value() );

    const auto button_down =
        std::ranges::find_if( events,
                              []( const grab::Event& event )
                              {
                                  return event.kind == grab::EventKind::MouseButtonDown;
                              } );
    ASSERT_NE( button_down, events.end() );
    EXPECT_EQ( button_down->origin, grab::EventOrigin::InjectedSelf );
    EXPECT_EQ( std::get<grab::MouseButton>( button_down->payload ).button, test_button );

    const auto button_up =
        std::ranges::find_if( events,
                              []( const grab::Event& event )
                              {
                                  return event.kind == grab::EventKind::MouseButtonUp;
                              } );
    ASSERT_NE( button_up, events.end() );
    EXPECT_EQ( button_up->origin, grab::EventOrigin::InjectedSelf );
    EXPECT_EQ( std::get<grab::MouseButton>( button_up->payload ).button, test_button );

    ASSERT_TRUE( runtime.stop().has_value() );
}

// GoogleTest assertion macros inflate the reported cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST( X11EventSource,
      ButtonDownAndUpCarryQueriedPositionInGlobalSpace )
{
    const char* const display = std::getenv( "DISPLAY" );
    if( display == nullptr || std::string_view{ display }.empty() )
    {
        GTEST_SKIP() << "requires Xvfb (DISPLAY is not set)";
    }

    grab::drivers::desktop::x11::X11Runtime runtime;
    const grab::OperationContext            start_context{
        .deadline = grab::Deadline::unbounded(),
    };
    ASSERT_TRUE( runtime.start( start_context ).has_value() );
    const auto* const capture_route = runtime.capture_route();
    ASSERT_NE( capture_route, nullptr );

    std::vector<grab::Event> events;
    runtime.set_event_sink(
        [&events]( grab::Event&& event )
        {
            events.push_back( std::move( event ) );
        }
    );

    auto* const event_source = runtime.event_source();
    ASSERT_NE( event_source, nullptr );
    const grab::spi::EventSpec down_spec{ std::string{ mouseButtonDownWireName } };
    const grab::spi::EventSpec up_spec{ std::string{ mouseButtonUpWireName } };
    ASSERT_TRUE( event_source->enable( down_spec ).has_value() );
    ASSERT_TRUE( event_source->enable( up_spec ).has_value() );

    auto* const seat = runtime.native_seat();
    ASSERT_NE( seat, nullptr );
    ASSERT_TRUE(
        seat->move_pointer_absolute( test_pointer_x, test_pointer_y ).has_value()
    );
    ASSERT_TRUE( seat->flush().has_value() );

    ASSERT_TRUE( seat->button( test_button, true ).has_value() );
    ASSERT_TRUE( seat->flush().has_value() );
    const grab::OperationContext down_wait_context{
        .deadline = grab::Deadline::after( event_wait_budget ),
    };
    ASSERT_TRUE( event_source
                     ->wait_for_event( down_spec, down_wait_context, event_wait_budget )
                     .has_value() );

    ASSERT_TRUE( seat->button( test_button, false ).has_value() );
    ASSERT_TRUE( seat->flush().has_value() );
    const grab::OperationContext up_wait_context{
        .deadline = grab::Deadline::after( event_wait_budget ),
    };
    ASSERT_TRUE( event_source
                     ->wait_for_event( up_spec, up_wait_context, event_wait_budget )
                     .has_value() );

    const auto expect_position = [capture_route]( const grab::Event& event )
    {
        const auto& payload = std::get<grab::MouseButton>( event.payload );
        ASSERT_TRUE( payload.position.has_value() );
        EXPECT_DOUBLE_EQ( payload.position->x, static_cast<double>( test_pointer_x ) );
        EXPECT_DOUBLE_EQ( payload.position->y, static_cast<double>( test_pointer_y ) );
        EXPECT_EQ( payload.position->space, capture_route->global_space() );
    };

    const auto button_down =
        std::ranges::find_if( events,
                              []( const grab::Event& event )
                              {
                                  return event.kind == grab::EventKind::MouseButtonDown;
                              } );
    ASSERT_NE( button_down, events.end() );
    expect_position( *button_down );

    const auto button_up =
        std::ranges::find_if( events,
                              []( const grab::Event& event )
                              {
                                  return event.kind == grab::EventKind::MouseButtonUp;
                              } );
    ASSERT_NE( button_up, events.end() );
    expect_position( *button_up );

    ASSERT_TRUE( runtime.stop().has_value() );
}

// GoogleTest assertion macros inflate the reported cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST( X11EventSource,
      ButtonPressPreservesLegacyMouseClickPayload )
{
    const char* const display = std::getenv( "DISPLAY" );
    if( display == nullptr || std::string_view{ display }.empty() )
    {
        GTEST_SKIP() << "requires Xvfb (DISPLAY is not set)";
    }

    grab::drivers::desktop::x11::X11Runtime runtime;
    const grab::OperationContext            start_context{
        .deadline = grab::Deadline::unbounded(),
    };
    ASSERT_TRUE( runtime.start( start_context ).has_value() );

    std::vector<grab::Event> events;
    runtime.set_event_sink(
        [&events]( grab::Event&& event )
        {
            events.push_back( std::move( event ) );
        }
    );

    auto* const event_source = runtime.event_source();
    ASSERT_NE( event_source, nullptr );
    const grab::spi::EventSpec click_spec{ std::string{ mouseClickWireName } };
    ASSERT_TRUE( event_source->enable( click_spec ).has_value() );

    auto* const seat = runtime.native_seat();
    ASSERT_NE( seat, nullptr );
    ASSERT_TRUE( seat->button( test_button, true ).has_value() );
    ASSERT_TRUE( seat->flush().has_value() );

    const grab::OperationContext wait_context{
        .deadline = grab::Deadline::after( event_wait_budget ),
    };
    ASSERT_TRUE( event_source
                     ->wait_for_event( click_spec, wait_context, event_wait_budget )
                     .has_value() );

    const auto click =
        std::ranges::find_if( events,
                              []( const grab::Event& event )
                              {
                                  return event.kind == grab::EventKind::MouseClick;
                              } );
    ASSERT_NE( click, events.end() );
    ASSERT_TRUE( std::holds_alternative<grab::MouseClick>( click->payload ) );
    const auto& payload = std::get<grab::MouseClick>( click->payload );
    EXPECT_EQ( payload.button, test_button );
    EXPECT_EQ( payload.name, emptyButtonName );

    ASSERT_TRUE( seat->button( test_button, false ).has_value() );
    ASSERT_TRUE( seat->flush().has_value() );
    ASSERT_TRUE( runtime.stop().has_value() );
}

// GoogleTest assertion macros inflate the reported cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST( X11EventSource,
      DemandSubscriptionTogglesXi2MaskDelivery )
{
    const char* const display = std::getenv( "DISPLAY" );
    if( display == nullptr || std::string_view{ display }.empty() )
    {
        GTEST_SKIP() << "requires Xvfb (DISPLAY is not set)";
    }

    auto opened_core =
        grab::kernel::lifecycle::SessionCore::open( grab::SessionOptions{}, nullptr );
    ASSERT_TRUE( opened_core.has_value() );
    auto core = std::move( *opened_core );
    ASSERT_NE( core, nullptr );

    EXPECT_EQ( core->bus().subscription_refcount( grab::EventKind::MouseClick ),
               no_subscriptions );

    std::optional<grab::Subscription> subscription;
    auto                              watched = core->watch( grab::SubscriptionScope{
        .kinds  = { grab::EventKind::MouseClick },
        .filter = {},
    } );
    ASSERT_TRUE( watched.has_value() );
    subscription.emplace( std::move( *watched ) );
    EXPECT_EQ( core->bus().subscription_refcount( grab::EventKind::MouseClick ),
               one_subscription );

    auto* const x11 = dynamic_cast<grab::drivers::desktop::x11::X11Runtime*>(
        &core->primary_runtime()
    );
    ASSERT_NE( x11, nullptr );
    auto* const seat = x11->native_seat();
    ASSERT_NE( seat, nullptr );

    ASSERT_TRUE( seat->button( test_button, true ).has_value() );
    ASSERT_TRUE( seat->button( test_button, false ).has_value() );
    ASSERT_TRUE( seat->flush().has_value() );

    auto* const event_source = x11->event_source();
    ASSERT_NE( event_source, nullptr );
    const grab::spi::EventSpec   spec{ "input.mouse_click" };
    const grab::OperationContext pump_context{
        .deadline = grab::Deadline::after( event_wait_budget ),
    };
    std::optional<grab::Event> delivered;
    for( std::size_t iteration{};
         iteration < maximum_pump_iterations && !delivered.has_value();
         ++iteration )
    {
        ASSERT_TRUE( event_source
                         ->wait_for_event( spec, pump_context, event_wait_budget )
                         .has_value() );
        delivered = subscription->try_pop();
    }

    ASSERT_TRUE( delivered.has_value() );
    EXPECT_EQ( delivered->kind, grab::EventKind::MouseClick );
    EXPECT_EQ( delivered->origin, grab::EventOrigin::InjectedSelf );

    while( subscription->try_pop().has_value() )
    {
    }
    subscription.reset();
    EXPECT_EQ( core->bus().subscription_refcount( grab::EventKind::MouseClick ),
               no_subscriptions );

    ASSERT_TRUE( seat->button( test_button, true ).has_value() );
    ASSERT_TRUE( seat->button( test_button, false ).has_value() );
    ASSERT_TRUE( seat->flush().has_value() );

    auto watched_again = core->watch( grab::SubscriptionScope{
        .kinds  = { grab::EventKind::MouseClick },
        .filter = {},
    } );
    ASSERT_TRUE( watched_again.has_value() );
    auto                         second_subscription = std::move( *watched_again );

    const grab::OperationContext short_context{
        .deadline = grab::Deadline::after( short_context_budget ),
    };
    static_cast<void>(
        event_source->wait_for_event( spec, short_context, short_wait_budget )
    );
    EXPECT_FALSE( second_subscription.try_pop().has_value() );
}

TEST( X11TopologySource,
      PollReturnsOutputsWithStableBaseline )
{
    const char* const display = std::getenv( "DISPLAY" );
    if( display == nullptr || std::string_view{ display }.empty() )
    {
        GTEST_SKIP() << "requires Xvfb (DISPLAY is not set)";
    }

    std::size_t                                    refresh_count{};
    grab::drivers::desktop::x11::X11TopologySource source{
        [&refresh_count]
        {
            ++refresh_count;
        },
    };

    const auto first = source.poll();
    ASSERT_TRUE( first.has_value() );
    EXPECT_FALSE( first->outputs.empty() );
    EXPECT_FALSE( first->changed );
    EXPECT_EQ( first->generation, initial_generation );

    const auto second = source.poll();
    ASSERT_TRUE( second.has_value() );
    EXPECT_FALSE( second->changed );
    EXPECT_EQ( second->generation, first->generation );
    EXPECT_EQ( second->outputs.size(), first->outputs.size() );
    EXPECT_EQ( refresh_count, no_refreshes );
}

// NOLINTEND(readability-trailing-comma)
