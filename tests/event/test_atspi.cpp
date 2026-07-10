#include "core/reactor.hpp"
#include "event/atspi.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/result.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <chrono>
#include <future>
#include <string_view>
#include <thread>
#include <variant>
// clang-format on

namespace
{

    constexpr double           timestamp          = 42.5;
    constexpr std::string_view objectInterface    = "org.a11y.atspi.Event.Object";
    constexpr std::string_view stateChangedMember = "StateChanged";
    constexpr std::string_view focusedDetail      = "focused";
    constexpr std::string_view pressedDetail      = "pressed";
    constexpr std::string_view unrelatedMember    = "BoundsChanged";
    constexpr std::string_view app                = "gedit";
    constexpr std::string_view buttonRole         = "push button";
    constexpr std::string_view buttonName         = "Save";
    constexpr auto             a11yCategory       = grab::EventCategory::Accessibility;
    constexpr auto             focusChangedKind   = grab::EventKind::A11yFocusChanged;
    constexpr auto             buttonClickedKind  = grab::EventKind::A11yButtonClicked;
    constexpr auto deviceInaccessibleCode         = grab::ErrorCode::DeviceInaccessible;
    constexpr auto threadReadyTimeout             = std::chrono::seconds{ 2 };
    constexpr std::string_view reactorDidNotStart = "reactor thread did not start";

    [[nodiscard]]
    grab::event::AtspiSignal
    atspi_signal( std::string_view member,
                  std::string_view detail )
    {
        return grab::event::AtspiSignal{
            .interface = std::string{ objectInterface },
            .member    = std::string{ member },
            .detail    = std::string{ detail },
            .app       = std::string{ app },
            .role      = std::string{ buttonRole },
            .name      = std::string{ buttonName },
        };
    }

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

}    // namespace

TEST( Atspi,
      DecodesFocusChange )
{
    const auto decoded =
        grab::event::decode_atspi_signal( atspi_signal( stateChangedMember,
                                                        focusedDetail ),
                                          timestamp );

    ASSERT_TRUE( decoded.has_value() );
    EXPECT_EQ( decoded->kind, focusChangedKind );
    EXPECT_EQ( decoded->category, a11yCategory );
    EXPECT_EQ( decoded->timestamp, timestamp );

    const auto* payload = std::get_if<grab::A11yEvent>( &decoded->payload );
    ASSERT_NE( payload, nullptr );
    EXPECT_EQ( payload->app, app );
    EXPECT_EQ( payload->role, buttonRole );
    EXPECT_EQ( payload->name, buttonName );
    EXPECT_EQ( payload->detail, focusedDetail );
}

TEST( Atspi,
      DecodesButtonClick )
{
    const auto decoded =
        grab::event::decode_atspi_signal( atspi_signal( stateChangedMember,
                                                        pressedDetail ),
                                          timestamp );

    ASSERT_TRUE( decoded.has_value() );
    EXPECT_EQ( decoded->kind, buttonClickedKind );
    EXPECT_EQ( decoded->category, a11yCategory );

    const auto* payload = std::get_if<grab::A11yEvent>( &decoded->payload );
    ASSERT_NE( payload, nullptr );
    EXPECT_EQ( payload->app, app );
    EXPECT_EQ( payload->role, buttonRole );
    EXPECT_EQ( payload->name, buttonName );
    EXPECT_EQ( payload->detail, pressedDetail );
}

TEST( Atspi,
      UnmappedSignalReturnsNullopt )
{
    const auto decoded =
        grab::event::decode_atspi_signal( atspi_signal( unrelatedMember, focusedDetail ),
                                          timestamp );

    EXPECT_FALSE( decoded.has_value() );
}

TEST( Atspi,
      StartFailsGracefullyWithoutA11yBus )
{
    RunningReactor running;
    ASSERT_TRUE( running.wait_until_started() ) << reactorDidNotStart;

    grab::EventBus bus;
    auto           monitor = grab::event::AtspiMonitor::start( running.reactor(), bus );

    if( monitor.has_value() )
    {
        // A real AT-SPI bus is present, so start() exercised the live D-Bus
        // subscription path. Registering libdbus's per-fd read+write watches
        // against the single-registration-per-fd reactor is the deferred
        // real-bus integration (documented follow-up), not this unit's concern:
        // this test verifies the no-bus graceful-failure contract.
        monitor->stop();
        running.stop_and_join();
        GTEST_SKIP()
            << "AT-SPI bus present; live subscription path is deferred integration";
    }

    EXPECT_EQ( monitor.error().code, deviceInaccessibleCode );
    running.stop_and_join();
    EXPECT_TRUE( running.result().has_value() );
}
