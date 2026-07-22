#include "fake/fake_runtime.hpp"
#include "grab/context.hpp"
#include "grab/result.hpp"
#include "grab/ui.hpp"
#include "kernel/action/wait_engine.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
// clang-format on

namespace
{

    constexpr auto waitBudget = std::chrono::seconds{ 1 };

}    // namespace

TEST( WaitEngine,
      TimeoutNamesPredicateAndPreservesLastFailureDiagnostic )
{
    grab::OperationContext context{
        .deadline = grab::Deadline::unbounded(),
    };
    grab::kernel::action::WaitEngine engine{ context };
    grab::testing::FakeEventSource   events;
    auto                             predicate = grab::kernel::action::NamedPredicate{
        .name    = "target_enabled",
        .observe = []() -> grab::Result<grab::kernel::action::PredicateObservation>
        {
            return grab::kernel::action::PredicateObservation{
                .satisfied = false,
                .detail    = "last observation: target was disabled",
            };
        },
    };

    const auto result = engine.wait( predicate,
                                     grab::kernel::action::WaitParams{
                                         .deadline =
                                             grab::Deadline{
                                                            .at = std::chrono::steady_clock::now(),
                                                            },
    },
                                     events );

    ASSERT_FALSE( result.has_value() );
    EXPECT_EQ( result.error().code, grab::ErrorCode::DeadlineExceeded );
    EXPECT_NE( result.error().message.find( "target_enabled" ), std::string::npos );
    ASSERT_FALSE( result.error().diagnostics.empty() );
    EXPECT_NE( result.error().diagnostics.back().message.find( "target was disabled" ),
               std::string::npos );
    EXPECT_EQ( events.demand_count( grab::spi::EventSpec{ "target_enabled" } ), 0U );
}

TEST( WaitEngine,
      ScriptedEventWakeReevaluatesPredicateWithoutSleeping )
{
    grab::OperationContext context{
        .deadline = grab::Deadline::after( waitBudget ),
    };
    grab::kernel::action::WaitEngine engine{ context };
    grab::testing::FakeEventSource   events;
    bool                             ready{};
    events.push_wakeup(
        [&ready]()
        {
            ready = true;
        }
    );
    auto predicate = grab::kernel::action::NamedPredicate{
        .name    = "node_present",
        .observe = [&ready]() -> grab::Result<grab::kernel::action::PredicateObservation>
        {
            return grab::kernel::action::PredicateObservation{
                .satisfied = ready,
                .detail    = ready ? "node present" : "node missing",
            };
        },
    };

    const auto result = engine.wait( predicate,
                                     grab::kernel::action::WaitParams{
                                         .deadline = context.deadline,
                                     },
                                     events );

    EXPECT_TRUE( result.has_value() );
    EXPECT_EQ( events.wait_count(), 1U );
    EXPECT_EQ( events.demand_count( grab::spi::EventSpec{ "node_present" } ), 0U );
}

TEST( WaitEngine,
      ComposesPresenceStabilityAndEnabledPredicates )
{
    grab::OperationContext context{
        .deadline = grab::Deadline::after( waitBudget ),
    };
    grab::kernel::action::WaitEngine engine{ context };
    grab::testing::FakeEventSource   events;
    events.push_wakeup(
        []()
        {
        }
    );

    const grab::kernel::action::NodeObserver observer =
        []() -> grab::Result<grab::kernel::action::NodeObservation>
    {
        return grab::kernel::action::NodeObservation{
            .present = true,
            .states  = grab::state_mask( grab::NodeState::Enabled ),
            .detail  = "control observed",
        };
    };
    std::vector<grab::kernel::action::NamedPredicate> checks;
    checks.push_back( grab::kernel::action::node_present( observer ) );
    checks.push_back( grab::kernel::action::state_stable( observer, 2U ) );
    checks.push_back( grab::kernel::action::enabled( observer ) );
    auto predicate = grab::kernel::action::all_of( "actionable", std::move( checks ) );

    const auto result = engine.wait( predicate,
                                     grab::kernel::action::WaitParams{
                                         .deadline = context.deadline,
                                     },
                                     events );

    EXPECT_TRUE( result.has_value() );
    EXPECT_EQ( events.wait_count(), 1U );
}

TEST( WaitEngine,
      WindowMappedSatisfiedWhenNodePresentAndVisible )
{
    grab::OperationContext context{
        .deadline = grab::Deadline::after( waitBudget ),
    };
    grab::kernel::action::WaitEngine engine{ context };
    grab::testing::FakeEventSource   events;
    events.push_wakeup(
        []()
        {
        }
    );

    const grab::kernel::action::NodeObserver observer =
        []() -> grab::Result<grab::kernel::action::NodeObservation>
    {
        return grab::kernel::action::NodeObservation{
            .present = true,
            .states  = grab::state_mask( grab::NodeState::Visible ),
            .detail  = "window observed",
        };
    };
    auto       predicate = grab::kernel::action::window_mapped( observer );

    const auto result    = engine.wait( predicate,
                                        grab::kernel::action::WaitParams{
                                            .deadline = context.deadline,
                                        },
                                        events );

    EXPECT_TRUE( result.has_value() );
}

TEST( WaitEngine,
      WindowMappedUnsatisfiedWhenNodeNotVisible )
{
    grab::OperationContext context{
        .deadline = grab::Deadline::unbounded(),
    };
    grab::kernel::action::WaitEngine         engine{ context };
    grab::testing::FakeEventSource           events;

    const grab::kernel::action::NodeObserver observer =
        []() -> grab::Result<grab::kernel::action::NodeObservation>
    {
        return grab::kernel::action::NodeObservation{
            .present = true,
            .states  = grab::state_mask( grab::NodeState::Enabled ),
            .detail  = "window is not on screen",
        };
    };
    auto       predicate = grab::kernel::action::window_mapped( observer );

    const auto result = engine.wait( predicate,
                                     grab::kernel::action::WaitParams{
                                         .deadline =
                                             grab::Deadline{
                                                            .at = std::chrono::steady_clock::now(),
                                                            },
    },
                                     events );

    ASSERT_FALSE( result.has_value() );
    EXPECT_EQ( result.error().code, grab::ErrorCode::DeadlineExceeded );
    EXPECT_NE( result.error().message.find( "window_mapped" ), std::string::npos );
}
