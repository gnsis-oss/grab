#include "core/reactor.hpp"
#include "event/fake_source.hpp"
#include "event/source.hpp"
#include "event/source_registry.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/result.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace
{

    constexpr std::string_view firstName       = "first";
    constexpr std::string_view failingName     = "failing";
    constexpr std::string_view secondName      = "second";
    constexpr std::string_view thirdName       = "third";
    constexpr std::string_view sourceFailure   = "source failed";
    constexpr auto             providerFailure = grab::ErrorCode::ProviderFailed;

    [[nodiscard]]
    grab::Result<void>
    failed_start_result()
    {
        return grab::fail( providerFailure, std::string{ sourceFailure } );
    }

    [[nodiscard]]
    std::unique_ptr<grab::test::FakeSource>
    make_source( std::string                  name,
                 std::vector<grab::EventKind> kinds = {} )
    {
        return std::make_unique<grab::test::FakeSource>( std::move( name ),
                                                         std::move( kinds ) );
    }

}    // namespace

TEST( SourceRegistry,
      StartAllStartsEverySourceAndDegradesFailures )
{
    grab::event::SourceRegistry registry;
    grab::core::Reactor         reactor;
    grab::EventBus              bus;

    auto first = make_source( std::string{ firstName }, { grab::EventKind::KeyDown } );
    auto failing =
        make_source( std::string{ failingName }, { grab::EventKind::WindowCreated } );
    auto second =
        make_source( std::string{ secondName }, { grab::EventKind::MouseMove } );
    failing->set_start_result( failed_start_result() );

    const auto* first_ptr   = first.get();
    const auto* failing_ptr = failing.get();
    const auto* second_ptr  = second.get();

    registry.add( std::move( first ) );
    registry.add( std::move( failing ) );
    registry.add( std::move( second ) );

    auto result = registry.start_all( reactor, bus );

    ASSERT_TRUE( result.has_value() ) << result.error().message;
    EXPECT_EQ( first_ptr->start_calls(), 1U );
    EXPECT_EQ( failing_ptr->start_calls(), 1U );
    EXPECT_EQ( second_ptr->start_calls(), 1U );
    EXPECT_EQ( first_ptr->state(), grab::event::SourceState::Running );
    EXPECT_EQ( failing_ptr->state(), grab::event::SourceState::Failed );
    EXPECT_EQ( second_ptr->state(), grab::event::SourceState::Running );
    EXPECT_TRUE( registry.is_kind_active( grab::EventKind::KeyDown ) );
    EXPECT_TRUE( registry.is_kind_active( grab::EventKind::MouseMove ) );
    EXPECT_FALSE( registry.is_kind_active( grab::EventKind::WindowCreated ) );

    const auto statuses = registry.statuses();
    ASSERT_EQ( statuses.size(), 3U );
    EXPECT_EQ( statuses.at( 0U ).name, firstName );
    EXPECT_EQ( statuses.at( 0U ).state, grab::event::SourceState::Running );
    EXPECT_EQ( statuses.at( 1U ).name, failingName );
    EXPECT_EQ( statuses.at( 1U ).state, grab::event::SourceState::Failed );
    EXPECT_EQ( statuses.at( 2U ).name, secondName );
    EXPECT_EQ( statuses.at( 2U ).state, grab::event::SourceState::Running );
}

TEST( SourceRegistry,
      StopAllStopsSourcesInReverseOrder )
{
    grab::event::SourceRegistry registry;
    std::vector<std::string>    stopped;

    auto                        first  = make_source( std::string{ firstName } );
    auto                        second = make_source( std::string{ secondName } );
    auto                        third  = make_source( std::string{ thirdName } );

    first->set_on_stop(
        [&stopped]( grab::test::FakeSource& source )
        {
            stopped.emplace_back( source.name() );
        }
    );
    second->set_on_stop(
        [&stopped]( grab::test::FakeSource& source )
        {
            stopped.emplace_back( source.name() );
        }
    );
    third->set_on_stop(
        [&stopped]( grab::test::FakeSource& source )
        {
            stopped.emplace_back( source.name() );
        }
    );

    registry.add( std::move( first ) );
    registry.add( std::move( second ) );
    registry.add( std::move( third ) );

    registry.stop_all();

    const std::vector<std::string> expected{
        std::string{ thirdName },
        std::string{ secondName },
        std::string{ firstName },
    };
    EXPECT_EQ( stopped, expected );
}
