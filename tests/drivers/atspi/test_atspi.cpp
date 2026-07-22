#include "drivers/semantic/atspi/atspi_monitor.hpp"
#include "grab/event.hpp"
#include "grab/result.hpp"
#include "kernel/events/event_bus.hpp"
#include "kernel/scheduling/reactor.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <chrono>
#include <future>
#include <string>
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
    constexpr std::string_view checkedDetail      = "checked";
    constexpr std::string_view showingDetail      = "showing";
    constexpr std::string_view defunctDetail      = "defunct";
    constexpr std::string_view unrelatedMember    = "BoundsChanged";
    constexpr std::string_view busSender          = ":1.1";
    constexpr std::string_view otherSender        = ":1.42";
    constexpr std::string_view friendlyApp        = "Firefox";
    constexpr std::string_view objectPathA        = "/org/a11y/atspi/accessible/1";
    constexpr std::string_view objectPathB        = "/org/a11y/atspi/accessible/2";
    constexpr std::string_view app                = "gedit";
    constexpr std::string_view buttonRole         = "push button";
    constexpr std::string_view buttonName         = "Save";
    constexpr auto             a11yCategory       = grab::EventCategory::Accessibility;
    constexpr auto             focusChangedKind   = grab::EventKind::A11yFocusChanged;
    constexpr auto             buttonClickedKind  = grab::EventKind::A11yButtonClicked;
    constexpr auto             stateChangedKind   = grab::EventKind::A11yStateChanged;
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

// The registry registration list is the vocabulary. StateChanged details we
// registered decode; anything else — most loudly "defunct", which fires for
// every accessible object an app destroys (file dialogs, list scrolling) —
// must not flood the bus with A11yStateChanged events.
TEST( Atspi,
      RegisteredStateDetailDecodesToStateChanged )
{
    const auto decoded =
        grab::event::decode_atspi_signal( atspi_signal( stateChangedMember,
                                                        checkedDetail ),
                                          timestamp );

    ASSERT_TRUE( decoded.has_value() );
    EXPECT_EQ( decoded->kind, stateChangedKind );
}

TEST( Atspi,
      UnregisteredStateDetailReturnsNullopt )
{
    const auto decoded =
        grab::event::decode_atspi_signal( atspi_signal( stateChangedMember,
                                                        defunctDetail ),
                                          timestamp );

    EXPECT_FALSE( decoded.has_value() );
}

// A resolved friendly application name replaces the raw D-Bus bus sender that
// live signals carry as their app; the emitting object itself carries no name.
TEST( Atspi,
      EnrichReplacesBusSenderWithFriendlyApp )
{
    auto signal = atspi_signal( stateChangedMember, showingDetail );
    signal.app  = std::string{ busSender };

    grab::event::enrich_atspi_signal(
        signal,
        grab::event::AtspiIdentity{ .app = std::string{ friendlyApp } }
    );

    EXPECT_EQ( signal.app, friendlyApp );
}

TEST( Atspi,
      EnrichWithoutIdentityKeepsBusSender )
{
    auto signal = atspi_signal( stateChangedMember, showingDetail );
    signal.app  = std::string{ busSender };

    grab::event::enrich_atspi_signal( signal, std::nullopt );

    EXPECT_EQ( signal.app, busSender );
}

TEST( Atspi,
      EnrichWithEmptyFriendlyNameKeepsBusSender )
{
    auto signal = atspi_signal( stateChangedMember, showingDetail );
    signal.app  = std::string{ busSender };

    grab::event::enrich_atspi_signal( signal, grab::event::AtspiIdentity{ .app = {} } );

    EXPECT_EQ( signal.app, busSender );
}

// Every accessible of one application shares its bus sender, so the friendly
// name is queried once per sender and served from cache thereafter — the
// property that keeps visibility-signal bursts from flooding the bus.
TEST( Atspi,
      IdentityCacheQueriesOncePerSender )
{
    int                             calls = 0;
    grab::event::AtspiIdentityCache cache(
        [&calls]( const grab::event::AtspiObjectRef& )
        {
            ++calls;
            return grab::event::AtspiIdentity{ .app = std::string{ friendlyApp } };
        }
    );

    const grab::event::AtspiObjectRef first{
        .sender = std::string{ busSender },
        .path   = std::string{ objectPathA }
    };
    const grab::event::AtspiObjectRef second{
        .sender = std::string{ busSender },
        .path   = std::string{ objectPathB }
    };
    const grab::event::AtspiObjectRef other{
        .sender = std::string{ otherSender },
        .path   = std::string{ objectPathA }
    };

    EXPECT_EQ( cache.resolve( first )->app, friendlyApp );
    EXPECT_EQ( cache.resolve( second )->app, friendlyApp );    // same sender: cached
    EXPECT_EQ( calls, 1 );

    EXPECT_EQ( cache.resolve( other )->app, friendlyApp );     // new sender: queried
    EXPECT_EQ( calls, 2 );
}

TEST( Atspi,
      IdentityCacheRemembersNullopt )
{
    int                             calls = 0;
    grab::event::AtspiIdentityCache cache(
        [&calls]( const grab::event::AtspiObjectRef& )
            -> std::optional<grab::event::AtspiIdentity>
        {
            ++calls;
            return std::nullopt;
        }
    );

    const grab::event::AtspiObjectRef object{
        .sender = std::string{ busSender },
        .path   = std::string{ objectPathA }
    };

    EXPECT_FALSE( cache.resolve( object ).has_value() );
    EXPECT_FALSE( cache.resolve( object ).has_value() );
    EXPECT_EQ( calls, 1 );    // an unresolvable sender is not re-queried per event
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

// GoogleTest assertion macros inflate the reported cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST( AtspiEventRegistry,
      DemandTogglesRegistrationExactlyOnceAtTransitions )
{
    int                             enable_calls  = 0;
    int                             disable_calls = 0;
    std::string                     last_name;
    grab::event::AtspiEventRegistry registry(
        [&]( std::string_view name, bool enable ) -> grab::Result<void>
        {
            last_name = std::string{ name };
            if( enable )
            {
                ++enable_calls;
            }
            else
            {
                ++disable_calls;
            }
            return {};
        }
    );

    constexpr std::string_view eventName = "object:state-changed:focused";

    ASSERT_TRUE( registry.acquire( eventName ).has_value() );    // 0->1 registers
    EXPECT_EQ( enable_calls, 1 );
    EXPECT_EQ( registry.demand( eventName ), 1U );

    ASSERT_TRUE( registry.acquire( eventName ).has_value() );    // 1->2 no register
    EXPECT_EQ( enable_calls, 1 );
    EXPECT_EQ( registry.demand( eventName ), 2U );

    registry.release( eventName );    // 2->1 no deregister
    EXPECT_EQ( disable_calls, 0 );
    EXPECT_EQ( registry.demand( eventName ), 1U );

    registry.release( eventName );    // 1->0 deregisters
    EXPECT_EQ( disable_calls, 1 );
    EXPECT_EQ( registry.demand( eventName ), 0U );
    EXPECT_EQ( last_name, std::string{ eventName } );
}
