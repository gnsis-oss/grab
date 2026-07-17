#include "kernel/support/environment.hpp"
#include "spi/monitor.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstdint>
// clang-format on

namespace
{

    constexpr std::uint64_t initialGeneration = 0U;
    constexpr std::uint64_t firstGeneration   = 1U;
    constexpr int           noNotifications   = 0;
    constexpr int           oneNotification   = 1;
    constexpr bool          callbackNotRun    = false;
    constexpr bool          callbackRan       = true;

}    // namespace

TEST( Monitor,
      UpdateBumpsGenerationAndNotifies )
{
    grab::core::EnvironmentMonitor monitor( grab::core::Environment{} );
    EXPECT_EQ( monitor.current().generation, initialGeneration );

    int           notified        = noNotifications;
    std::uint64_t seen_generation = initialGeneration;
    const auto    id              = monitor.subscribe(
        [&]( const grab::core::Environment& env )
        {
            ++notified;
            seen_generation = env.generation;
        }
    );

    grab::core::Environment next;
    next.session          = grab::core::SessionType::X11;
    const auto generation = monitor.update( next );

    EXPECT_EQ( generation, firstGeneration );
    EXPECT_EQ( monitor.current().generation, firstGeneration );
    EXPECT_EQ( monitor.current().session, grab::core::SessionType::X11 );
    EXPECT_EQ( notified, oneNotification );
    EXPECT_EQ( seen_generation, firstGeneration );

    monitor.unsubscribe( id );
    monitor.update( grab::core::Environment{} );
    EXPECT_EQ( notified, oneNotification );
}

TEST( Monitor,
      ListenersRunOutsideTheLock )
{
    grab::core::EnvironmentMonitor monitor( grab::core::Environment{} );

    bool                           callback_ran        = callbackNotRun;
    std::uint64_t                  notified_generation = initialGeneration;
    std::uint64_t                  current_generation  = initialGeneration;
    const auto                     id                  = monitor.subscribe(
        [&]( const grab::core::Environment& env )
        {
            callback_ran         = callbackRan;
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

    EXPECT_EQ( generation, firstGeneration );
    EXPECT_EQ( callback_ran, callbackRan );
    EXPECT_EQ( notified_generation, generation );
    EXPECT_EQ( current_generation, notified_generation );

    monitor.unsubscribe( id );
}
