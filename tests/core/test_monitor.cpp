#include "core/environment.hpp"
#include "core/monitor.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstdint>
// clang-format on

namespace
{

    constexpr std::uint64_t kInitialGeneration = 0U;
    constexpr std::uint64_t kFirstGeneration   = 1U;
    constexpr int           kNoNotifications   = 0;
    constexpr int           kOneNotification   = 1;
    constexpr bool          kCallbackNotRun    = false;
    constexpr bool          kCallbackRan       = true;

}    // namespace

TEST( Monitor,
      UpdateBumpsGenerationAndNotifies )
{
    grab::core::EnvironmentMonitor monitor( grab::core::Environment{} );
    EXPECT_EQ( monitor.current().generation, kInitialGeneration );

    int           notified        = kNoNotifications;
    std::uint64_t seen_generation = kInitialGeneration;
    const auto    id              = monitor.subscribe(
        [&]( const grab::core::Environment& env )
        {
            ++notified;
            seen_generation = env.generation;
        }
    );

    grab::core::Environment next;
    next.session          = grab::core::SessionType::x11;
    const auto generation = monitor.update( next );

    EXPECT_EQ( generation, kFirstGeneration );
    EXPECT_EQ( monitor.current().generation, kFirstGeneration );
    EXPECT_EQ( monitor.current().session, grab::core::SessionType::x11 );
    EXPECT_EQ( notified, kOneNotification );
    EXPECT_EQ( seen_generation, kFirstGeneration );

    monitor.unsubscribe( id );
    monitor.update( grab::core::Environment{} );
    EXPECT_EQ( notified, kOneNotification );
}

TEST( Monitor,
      ListenersRunOutsideTheLock )
{
    grab::core::EnvironmentMonitor monitor( grab::core::Environment{} );

    bool                           callback_ran        = kCallbackNotRun;
    std::uint64_t                  notified_generation = kInitialGeneration;
    std::uint64_t                  current_generation  = kInitialGeneration;
    const auto                     id                  = monitor.subscribe(
        [&]( const grab::core::Environment& env )
        {
            callback_ran         = kCallbackRan;
            notified_generation  = env.generation;
            current_generation   = monitor.current().generation;

            const auto nested_id = monitor.subscribe(
                []( const grab::core::Environment& )
                {
                }
            );
            monitor.unsubscribe( nested_id );
        }
    );

    const auto generation = monitor.update( grab::core::Environment{} );

    EXPECT_EQ( generation, kFirstGeneration );
    EXPECT_EQ( callback_ran, kCallbackRan );
    EXPECT_EQ( notified_generation, generation );
    EXPECT_EQ( current_generation, notified_generation );

    monitor.unsubscribe( id );
}
