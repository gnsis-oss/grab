#include "core/reactor.hpp"
#include "event/xinput2.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/result.hpp"
#include "input/seat.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
// clang-format on

namespace
{

    constexpr const char*   xvfbDisplay            = ":97";
    constexpr const char*   badDisplay             = ":bad-nonexistent-97";
    constexpr auto          deviceInaccessibleCode = grab::ErrorCode::DeviceInaccessible;
    constexpr std::uint8_t  injectedKeycode        = 38U;
    constexpr std::uint32_t expectedKeycode        = injectedKeycode;
    constexpr std::size_t   subscriptionDepth      = 32U;
    constexpr auto          threadReadyTimeout     = std::chrono::seconds{ 2 };
    constexpr auto          registrationTimeout    = std::chrono::seconds{ 2 };
    constexpr auto          eventTimeout           = std::chrono::seconds{ 2 };
    constexpr auto          pollInterval           = std::chrono::milliseconds{ 10 };
    constexpr std::string_view reactorDidNotStart  = "reactor thread did not start";
    constexpr std::string_view monitorDidNotRegister = "monitor fd did not register";

    class RunningReactor
    {
        public:

            RunningReactor() :
                started_( reactor_started_.get_future() ),
                thread_(
                    [this]
                    {
                        reactor_started_.set_value();
                        result_ = reactor_.run();
                    }
                )
            {
            }

            ~RunningReactor()
            {
                stop_and_join();
            }

            RunningReactor( const RunningReactor& ) = delete;
            RunningReactor&
            operator=( const RunningReactor& ) = delete;
            RunningReactor( RunningReactor&& ) = delete;
            RunningReactor&
            operator=( RunningReactor&& ) = delete;

            [[nodiscard]]
            bool
            wait_until_started()
            {
                return started_.wait_for( threadReadyTimeout ) ==
                       std::future_status::ready;
            }

            [[nodiscard]]
            grab::core::Reactor&
            reactor() noexcept
            {
                return reactor_;
            }

            void
            stop_and_join() noexcept
            {
                reactor_.stop();
                if( thread_.joinable() )
                {
                    thread_.join();
                }
            }

            [[nodiscard]]
            const grab::Result<void>&
            result() const noexcept
            {
                return result_;
            }

        private:

            grab::core::Reactor reactor_;
            std::promise<void>  reactor_started_;
            std::future<void>   started_;
            grab::Result<void>  result_;
            std::thread         thread_;
    };

    [[nodiscard]]
    bool
    wait_for_reactor_barrier( grab::core::Reactor& reactor )
    {
        std::promise<void> registered;
        auto               registered_future = registered.get_future();
        reactor.post(
            [&registered]
            {
                registered.set_value();
            }
        );
        return registered_future.wait_for( registrationTimeout ) ==
               std::future_status::ready;
    }

    [[nodiscard]]
    std::optional<grab::Event>
    wait_for_event( grab::Subscription& subscription,
                    grab::EventKind     kind )
    {
        const auto deadline = std::chrono::steady_clock::now() + eventTimeout;
        while( std::chrono::steady_clock::now() < deadline )
        {
            while( auto event = subscription.try_pop() )
            {
                if( event->kind == kind )
                {
                    return event;
                }
            }
            std::this_thread::sleep_for( pollInterval );
        }
        return std::nullopt;
    }

}    // namespace

TEST( XInput2,
      ObservesInjectedKey )
{
    RunningReactor running;
    ASSERT_TRUE( running.wait_until_started() ) << reactorDidNotStart;

    grab::EventBus bus;
    auto           subscription = bus.subscribe(
        grab::EventFilter{
            .kinds      = { grab::EventKind::KeyDown },
            .categories = {},
        },
        subscriptionDepth
    );

    auto monitor_result =
        grab::event::XInput2Monitor::start( xvfbDisplay, running.reactor(), bus );
    ASSERT_TRUE( monitor_result.has_value() ) << monitor_result.error().message;
    auto monitor = std::move( *monitor_result );
    ASSERT_TRUE( wait_for_reactor_barrier( running.reactor() ) )
        << monitorDidNotRegister;

    auto seat = grab::input::Seat::open( xvfbDisplay );
    ASSERT_TRUE( seat.has_value() ) << seat.error().message;
    ASSERT_TRUE( seat->key( injectedKeycode, true ).has_value() );
    ASSERT_TRUE( seat->key( injectedKeycode, false ).has_value() );
    ASSERT_TRUE( seat->flush().has_value() );

    auto event = wait_for_event( subscription, grab::EventKind::KeyDown );
    ASSERT_TRUE( event.has_value() );
    const auto* payload = std::get_if<grab::InputKey>( &event->payload );
    ASSERT_NE( payload, nullptr );
    EXPECT_EQ( payload->code, expectedKeycode );

    monitor.stop();
    running.stop_and_join();
    EXPECT_TRUE( running.result().has_value() );
}

TEST( XInput2,
      StartFailsOnBadDisplay )
{
    grab::core::Reactor reactor;
    grab::EventBus      bus;

    auto monitor = grab::event::XInput2Monitor::start( badDisplay, reactor, bus );

    ASSERT_FALSE( monitor.has_value() );
    EXPECT_EQ( monitor.error().code, deviceInaccessibleCode );
}
