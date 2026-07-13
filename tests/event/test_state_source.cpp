#include "core/reactor.hpp"
#include "event/source.hpp"
#include "event/state_source.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/event_descriptor.hpp"
#include "grab/pid.hpp"
#include "grab/result.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
// clang-format on

namespace
{

    constexpr auto          windowCreatedKind      = grab::EventKind::WindowCreated;
    constexpr auto          windowClosedKind       = grab::EventKind::WindowClosed;
    constexpr auto          windowFocusChangedKind = grab::EventKind::WindowFocusChanged;
    constexpr auto          windowTitleChangedKind = grab::EventKind::WindowTitleChanged;
    constexpr auto          stateSnapshotKind      = grab::EventKind::StateSnapshot;
    constexpr double        timestamp              = 12.5;
    constexpr std::uint64_t unsetSequence          = 0U;
    constexpr std::string_view firstApp            = "editor";
    constexpr std::int64_t     firstPidValue       = 1'001;
    constexpr grab::Pid        firstPid{ firstPidValue };
    constexpr std::string_view firstTitle     = "main.cpp";
    constexpr std::string_view secondApp      = "browser";
    constexpr std::int64_t     secondPidValue = 1'002;
    constexpr grab::Pid        secondPid{ secondPidValue };
    constexpr std::string_view secondTitle           = "Release notes";
    constexpr std::string_view emptySnapshotJson     = R"({"open":[]})";
    constexpr auto             sourceInterval        = std::chrono::milliseconds{ 20 };
    constexpr auto             reactorStartTimeout   = std::chrono::seconds{ 2 };
    constexpr auto             snapshotWaitTimeout   = std::chrono::seconds{ 2 };
    constexpr auto             snapshotPollInterval  = std::chrono::milliseconds{ 5 };
    constexpr auto             cadenceObserveWindow  = std::chrono::milliseconds{ 180 };
    constexpr std::size_t      minimumCadenceSamples = 3U;
    constexpr std::size_t      maximumCadenceSamples = 20U;

    [[nodiscard]]
    grab::Event
    make_window_event( grab::EventKind  kind,
                       std::string_view app,
                       grab::Pid        pid,
                       std::string_view title )
    {
        return grab::Event{
            .timestamp = timestamp,
            .sequence  = unsetSequence,
            .kind      = kind,
            .category  = grab::category_of( kind ),
            .payload   = grab::Payload{ grab::WindowChange{
                .app        = std::string{ app },
                .pid        = pid,
                .title      = std::string{ title },
                .prev_title = {},
                .duration_s = 0.0,
            } },
        };
    }

    [[nodiscard]]
    grab::Event
    make_state_snapshot_event()
    {
        return grab::Event{
            .timestamp = timestamp,
            .sequence  = unsetSequence,
            .kind      = stateSnapshotKind,
            .category  = grab::category_of( stateSnapshotKind ),
            .payload   = grab::Payload{ grab::StateSnapshot{
                .json = std::string{ emptySnapshotJson },
            } },
        };
    }

    [[nodiscard]]
    const grab::StateSnapshot*
    state_snapshot_payload( const grab::Event& event )
    {
        return std::get_if<grab::StateSnapshot>( &event.payload );
    }

    class ReactorThread
    {
        public:

            ReactorThread() :
                started_( started_promise_.get_future().share() ),
                thread_(
                    [this]
                    {
                        started_promise_.set_value();
                        result_ = reactor_.run();
                    }
                )
            {
            }

            ~ReactorThread() noexcept
            {
                stop();
            }

            ReactorThread( const ReactorThread& ) = delete;
            ReactorThread&
            operator=( const ReactorThread& ) = delete;
            ReactorThread( ReactorThread&& )  = delete;
            ReactorThread&
            operator=( ReactorThread&& ) = delete;

            [[nodiscard]]
            bool
            wait_until_started() const
            {
                return started_.wait_for( reactorStartTimeout ) ==
                       std::future_status::ready;
            }

            [[nodiscard]]
            grab::core::Reactor&
            reactor() noexcept
            {
                return reactor_;
            }

            void
            stop() noexcept
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

            grab::core::Reactor      reactor_;
            grab::Result<void>       result_{};
            std::promise<void>       started_promise_;
            std::shared_future<void> started_;
            std::thread              thread_;
    };

    [[nodiscard]]
    bool
    wait_for_snapshot( grab::Subscription& subscription,
                       grab::Event&        snapshot,
                       std::string_view    first_needle,
                       std::string_view    second_needle )
    {
        const auto deadline = std::chrono::steady_clock::now() + snapshotWaitTimeout;
        while( std::chrono::steady_clock::now() < deadline )
        {
            while( std::optional<grab::Event> event = subscription.try_pop() )
            {
                const auto* payload = state_snapshot_payload( *event );
                if( payload == nullptr )
                {
                    continue;
                }

                if( payload->json.contains( first_needle ) &&
                    payload->json.contains( second_needle ) )
                {
                    snapshot = std::move( *event );
                    return true;
                }
            }

            std::this_thread::sleep_for( snapshotPollInterval );
        }

        return false;
    }

    [[nodiscard]]
    std::size_t
    count_snapshots_for( grab::Subscription&                 subscription,
                         std::chrono::steady_clock::duration duration )
    {
        const auto  deadline = std::chrono::steady_clock::now() + duration;
        std::size_t count    = 0U;
        while( std::chrono::steady_clock::now() < deadline )
        {
            while( std::optional<grab::Event> event = subscription.try_pop() )
            {
                if( state_snapshot_payload( *event ) != nullptr )
                {
                    ++count;
                }
            }

            std::this_thread::sleep_for( snapshotPollInterval );
        }
        return count;
    }

}    // namespace

TEST( StateSource,
      DefaultIntervalIsVisible )
{
    EXPECT_EQ( grab::event::defaultStateSourceInterval, std::chrono::seconds{ 60 } );
}

TEST( StateSource,
      FeedbackLoopFilterExcludesStateSnapshot )
{
    const auto filter = grab::event::detail::state_source_filter();

    EXPECT_TRUE( filter.matches(
        make_window_event( windowCreatedKind, firstApp, firstPid, firstTitle )
    ) );
    EXPECT_TRUE( filter.matches(
        make_window_event( windowClosedKind, firstApp, firstPid, firstTitle )
    ) );
    EXPECT_TRUE( filter.matches(
        make_window_event( windowFocusChangedKind, firstApp, firstPid, firstTitle )
    ) );
    EXPECT_FALSE( filter.matches(
        make_window_event( windowTitleChangedKind, firstApp, firstPid, firstTitle )
    ) );
    EXPECT_FALSE( filter.matches( make_state_snapshot_event() ) );
}

TEST( StateSource,
      ObservesWindowEventsAndRepublishesSnapshot )
{
    grab::EventBus bus;
    ReactorThread  reactor;
    ASSERT_TRUE( reactor.wait_until_started() );

    auto                     snapshots = bus.subscribe( grab::EventFilter{
        .kinds      = { stateSnapshotKind },
        .categories = {},
    } );
    grab::event::StateSource source{ sourceInterval };
    const auto               start_result = source.start( reactor.reactor(), bus );
    ASSERT_TRUE( start_result.has_value() ) << start_result.error().message;

    bus.publish(
        make_window_event( windowCreatedKind, firstApp, firstPid, firstTitle )
    );
    bus.publish(
        make_window_event( windowCreatedKind, secondApp, secondPid, secondTitle )
    );

    grab::Event snapshot;
    ASSERT_TRUE( wait_for_snapshot( snapshots, snapshot, firstTitle, secondTitle ) );
    EXPECT_EQ( snapshot.kind, stateSnapshotKind );

    source.stop();
    reactor.stop();
    EXPECT_TRUE( reactor.result().has_value() );
}

TEST( StateSource,
      DoesNotObserveOwnSnapshots )
{
    grab::EventBus bus;
    ReactorThread  reactor;
    ASSERT_TRUE( reactor.wait_until_started() );

    auto                     snapshots = bus.subscribe( grab::EventFilter{
        .kinds      = { stateSnapshotKind },
        .categories = {},
    } );
    grab::event::StateSource source{ sourceInterval };
    const auto               start_result = source.start( reactor.reactor(), bus );
    ASSERT_TRUE( start_result.has_value() ) << start_result.error().message;

    const std::size_t count = count_snapshots_for( snapshots, cadenceObserveWindow );

    EXPECT_EQ( source.state(), grab::event::SourceState::Running );
    EXPECT_GE( count, minimumCadenceSamples );
    EXPECT_LE( count, maximumCadenceSamples );

    source.stop();
    reactor.stop();
    EXPECT_TRUE( reactor.result().has_value() );
}

TEST( StateSource,
      StopIsCleanAndIdempotent )
{
    grab::EventBus bus;
    ReactorThread  reactor;
    ASSERT_TRUE( reactor.wait_until_started() );

    grab::event::StateSource source{ sourceInterval };
    const auto               start_result = source.start( reactor.reactor(), bus );
    ASSERT_TRUE( start_result.has_value() ) << start_result.error().message;

    source.stop();
    source.stop();

    EXPECT_EQ( source.state(), grab::event::SourceState::Stopped );

    reactor.stop();
    EXPECT_TRUE( reactor.result().has_value() );
}
