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

    constexpr double           kTimestamp          = 42.5;
    constexpr std::string_view kObjectInterface    = "org.a11y.atspi.Event.Object";
    constexpr std::string_view kStateChangedMember = "StateChanged";
    constexpr std::string_view kFocusedDetail      = "focused";
    constexpr std::string_view kPressedDetail      = "pressed";
    constexpr std::string_view kUnrelatedMember    = "BoundsChanged";
    constexpr std::string_view kApp                = "gedit";
    constexpr std::string_view kButtonRole         = "push button";
    constexpr std::string_view kButtonName         = "Save";
    constexpr auto             kA11yCategory       = grab::EventCategory::accessibility;
    constexpr auto             kFocusChangedKind   = grab::EventKind::a11y_focus_changed;
    constexpr auto             kButtonClickedKind = grab::EventKind::a11y_button_clicked;
    constexpr auto kDeviceInaccessibleCode        = grab::ErrorCode::device_inaccessible;
    constexpr auto kThreadReadyTimeout            = std::chrono::seconds{ 2 };
    constexpr std::string_view kReactorDidNotStart = "reactor thread did not start";

    [[nodiscard]]
    grab::event::AtspiSignal
    atspi_signal( std::string_view member,
                  std::string_view detail )
    {
        return grab::event::AtspiSignal{
            .interface = std::string{ kObjectInterface },
            .member    = std::string{ member },
            .detail    = std::string{ detail },
            .app       = std::string{ kApp },
            .role      = std::string{ kButtonRole },
            .name      = std::string{ kButtonName },
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
                return started_.wait_for( kThreadReadyTimeout ) ==
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
        grab::event::decode_atspi_signal( atspi_signal( kStateChangedMember,
                                                        kFocusedDetail ),
                                          kTimestamp );

    ASSERT_TRUE( decoded.has_value() );
    EXPECT_EQ( decoded->kind, kFocusChangedKind );
    EXPECT_EQ( decoded->category, kA11yCategory );
    EXPECT_EQ( decoded->timestamp, kTimestamp );

    const auto* payload = std::get_if<grab::A11yEvent>( &decoded->payload );
    ASSERT_NE( payload, nullptr );
    EXPECT_EQ( payload->app, kApp );
    EXPECT_EQ( payload->role, kButtonRole );
    EXPECT_EQ( payload->name, kButtonName );
    EXPECT_EQ( payload->detail, kFocusedDetail );
}

TEST( Atspi,
      DecodesButtonClick )
{
    const auto decoded =
        grab::event::decode_atspi_signal( atspi_signal( kStateChangedMember,
                                                        kPressedDetail ),
                                          kTimestamp );

    ASSERT_TRUE( decoded.has_value() );
    EXPECT_EQ( decoded->kind, kButtonClickedKind );
    EXPECT_EQ( decoded->category, kA11yCategory );

    const auto* payload = std::get_if<grab::A11yEvent>( &decoded->payload );
    ASSERT_NE( payload, nullptr );
    EXPECT_EQ( payload->app, kApp );
    EXPECT_EQ( payload->role, kButtonRole );
    EXPECT_EQ( payload->name, kButtonName );
    EXPECT_EQ( payload->detail, kPressedDetail );
}

TEST( Atspi,
      UnmappedSignalReturnsNullopt )
{
    const auto decoded =
        grab::event::decode_atspi_signal( atspi_signal( kUnrelatedMember,
                                                        kFocusedDetail ),
                                          kTimestamp );

    EXPECT_FALSE( decoded.has_value() );
}

TEST( Atspi,
      StartFailsGracefullyWithoutA11yBus )
{
    RunningReactor running;
    ASSERT_TRUE( running.wait_until_started() ) << kReactorDidNotStart;

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

    EXPECT_EQ( monitor.error().code, kDeviceInaccessibleCode );
    running.stop_and_join();
    EXPECT_TRUE( running.result().has_value() );
}
