// The suite name is load-bearing: tests/CMakeLists.txt labels
// "TimerThreadTiming.*" with LABELS "timing", so a loaded machine can exclude
// these with `ctest -LE timing`. Renaming the suite silently unbinds the label.
//
// These tests exist to prove one thing the design (§4.4) calls structural: the
// component that owns deadlines runs on its own thread and cannot be delayed by
// whatever the caller is doing. A pull-only implementation — arm(deadline) plus
// a `deadline <= now` filter over a caller-supplied clock — passes a naive
// "drain returns the token eventually" test while asserting nothing. The wake
// path is what is under test here, so every deadline assertion is made against
// wake_fd() readability observed from a thread that is deliberately busy.

#include "kernel/scheduling/timer_thread.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <poll.h>
#include <system_error>
#include <thread>
#include <vector>
// clang-format on

namespace
{

    using Clock                                = std::chrono::steady_clock;
    using TimerThread                          = grab::kernel::scheduling::TimerThread;
    using Token                                = TimerThread::Token;

    constexpr ::nfds_t       singleDescriptor  = 1U;
    constexpr int            noWait            = 0;
    constexpr int            noDescriptorReady = 0;
    constexpr int            noPollEvents      = 0;
    constexpr int            lowestValidDescriptor = 0;

    constexpr std::size_t    firstIndex            = 0U;
    constexpr std::size_t    oneToken              = 1U;
    constexpr std::ptrdiff_t noSightings           = 0;
    constexpr std::ptrdiff_t oneSighting           = 1;

    // Deadlines and budgets. Generous on purpose: the machine is shared and a
    // scheduler under load will not deliver microsecond accuracy. The
    // assertions are about ordering and about the wake path existing, not about
    // scheduler precision.
    constexpr auto           pollInterval    = std::chrono::milliseconds{ 2 };
    constexpr auto           shortDeadline   = std::chrono::milliseconds{ 50 };
    constexpr auto           wakeBudget      = std::chrono::milliseconds{ 2'000 };
    constexpr auto           cancelDeadline  = std::chrono::milliseconds{ 40 };
    constexpr auto           observationSpan = std::chrono::milliseconds{ 250 };

    // The ceiling-3 assertion (§4.7): a 5 ms deadline observed from a thread
    // that is occupied for 50 ms without sleeping, yielding or blocking.
    constexpr auto           wakeDeadline     = std::chrono::milliseconds{ 5 };
    constexpr auto           busyWaitSpan     = std::chrono::milliseconds{ 50 };
    constexpr auto           wakeTolerance    = std::chrono::milliseconds{ 40 };

    constexpr auto           stopDeadline     = std::chrono::milliseconds{ 40 };
    constexpr auto           postStopSpan     = std::chrono::milliseconds{ 200 };
    constexpr auto           farDeadline      = std::chrono::hours{ 1 };
    constexpr auto           teardownBudget   = std::chrono::seconds{ 5 };
    constexpr std::size_t    armedTimerCount  = 64U;
    constexpr std::size_t    lifecycleCycles  = 16U;

    constexpr std::size_t    staggeredCount   = 5U;
    constexpr auto           staggeredStep    = std::chrono::milliseconds{ 12 };
    constexpr auto           staggeredInitial = std::chrono::milliseconds{ 20 };
    constexpr auto           staggeredBudget  = std::chrono::milliseconds{ 3'000 };

    [[nodiscard]]
    bool
    readable( ::pollfd& descriptor,
              int       timeout_ms )
    {
        const int ready = ::poll( &descriptor, singleDescriptor, timeout_ms );
        return ready >
               noDescriptorReady &&
               ( descriptor.revents & POLLIN ) != noPollEvents;
    }

    // Non-blocking: asks whether the fd is readable right now and returns
    // immediately either way.
    [[nodiscard]]
    bool
    readable_now( int fd )
    {
        ::pollfd descriptor{};
        descriptor.fd     = fd;
        descriptor.events = static_cast<short>( POLLIN );
        return readable( descriptor, noWait );
    }

    [[nodiscard]]
    bool
    wait_readable( int                       fd,
                   std::chrono::milliseconds budget )
    {
        ::pollfd descriptor{};
        descriptor.fd     = fd;
        descriptor.events = static_cast<short>( POLLIN );
        return readable( descriptor, static_cast<int>( budget.count() ) );
    }

    // Descriptors are the resource this class is most likely to leak: it owns
    // three of them and a thread. /proc/self/fd is the direct witness.
    [[nodiscard]]
    std::size_t
    open_descriptor_count()
    {
        std::size_t     count = firstIndex;
        std::error_code failure;
        for( const auto& entry :
             std::filesystem::directory_iterator{ "/proc/self/fd", failure } )
        {
            static_cast<void>( entry );
            ++count;
        }
        return count;
    }

}    // namespace

TEST( TimerThreadTiming,
      WakeFdIsAnOwnedDescriptorAndIdleUntilSomethingIsDue )
{
    const TimerThread timers;
    ASSERT_GE( timers.wake_fd(), lowestValidDescriptor );
    EXPECT_FALSE( readable_now( timers.wake_fd() ) );
}

TEST( TimerThreadTiming,
      DrainYieldsTheTokenOnlyAfterItsDeadline )
{
    TimerThread timers;
    const auto  deadline = Clock::now() + shortDeadline;
    const auto  token    = timers.arm( deadline );

    // Before the deadline: nothing due, and the fd is not readable. The
    // second assertion proves the first one was actually taken early rather
    // than after an unlucky scheduling gap.
    EXPECT_TRUE( timers.drain().empty() );
    EXPECT_FALSE( readable_now( timers.wake_fd() ) );
    EXPECT_LT( Clock::now(), deadline );

    ASSERT_TRUE( wait_readable( timers.wake_fd(), wakeBudget ) );
    EXPECT_GE( Clock::now(), deadline );

    const auto due = timers.drain();
    ASSERT_EQ( due.size(), oneToken );
    EXPECT_EQ( due.front(), token );

    // drain() takes the tokens and clears the readability in one step.
    EXPECT_TRUE( timers.drain().empty() );
    EXPECT_FALSE( readable_now( timers.wake_fd() ) );
}

TEST( TimerThreadTiming,
      CancelBeforeTheDeadlineMeansTheTokenNeverAppears )
{
    TimerThread timers;
    const auto  cancelled_at = Clock::now() + cancelDeadline;
    const auto  cancelled    = timers.arm( cancelled_at );
    const auto  kept         = timers.arm( cancelled_at + cancelDeadline );
    timers.cancel( cancelled );

    // Watch well past both deadlines. The surviving token is the control: if
    // it never arrives either, the test proves nothing about cancel().
    std::vector<Token> seen;
    const auto         until = Clock::now() + observationSpan;
    while( Clock::now() < until )
    {
        for( const Token token : timers.drain() )
        {
            seen.push_back( token );
        }
        std::this_thread::sleep_for( pollInterval );
    }

    EXPECT_EQ( std::ranges::count( seen, cancelled ), noSightings );
    EXPECT_EQ( std::ranges::count( seen, kept ), oneSighting );
}

// THE CEILING-3 ASSERTION.
//
// §4.7 puts blocking work sharing the timing thread at up to ~80 ms — the
// dominant precision term, 80x the millisecond rounding it dwarfs. This is the
// proof that grab's deadlines do not live on the caller's thread: the caller
// occupies itself for 50 ms straight, and a 5 ms deadline still lands on time.
//
// The busy wait never sleeps, never yields and never blocks, and the loop does
// not break when it observes readability — the full 50 ms of occupancy really
// happens. Every probe is a zero-timeout poll, so the only way this fd can
// become readable during the spin is a second thread making it so.
TEST( TimerThreadTiming,
      WakeFdFiresOnTimeWhileTheCallerIsBusyForTenTimesTheDeadline )
{
    TimerThread                      timers;
    const auto                       armed_at = Clock::now();
    const auto                       deadline = armed_at + wakeDeadline;
    const auto                       token    = timers.arm( deadline );

    std::optional<Clock::time_point> first_readable;
    const auto                       busy_until = armed_at + busyWaitSpan;
    while( Clock::now() < busy_until )
    {
        if( !first_readable.has_value() && readable_now( timers.wake_fd() ) )
        {
            first_readable = Clock::now();
        }
    }

    ASSERT_TRUE( first_readable.has_value() );
    EXPECT_GE( *first_readable, deadline );
    EXPECT_LE( *first_readable, deadline + wakeTolerance );

    const auto due = timers.drain();
    ASSERT_EQ( due.size(), oneToken );
    EXPECT_EQ( due.front(), token );
}

TEST( TimerThreadTiming,
      StaggeredDeadlinesAllExpireInDeadlineOrder )
{
    TimerThread        timers;
    std::vector<Token> armed;
    const auto         base = Clock::now() + staggeredInitial;
    for( std::size_t index = firstIndex; index < staggeredCount; ++index )
    {
        armed.push_back(
            timers.arm( base + ( staggeredStep *
                                 static_cast<std::chrono::milliseconds::rep>( index ) ) )
        );
    }

    std::vector<Token> seen;
    const auto         until = Clock::now() + staggeredBudget;
    while( seen.size() < staggeredCount && Clock::now() < until )
    {
        if( !wait_readable( timers.wake_fd(), pollInterval ) )
        {
            continue;
        }
        for( const Token token : timers.drain() )
        {
            seen.push_back( token );
        }
    }

    EXPECT_EQ( seen, armed );
}

TEST( TimerThreadTiming,
      StopEndsTheWorkerAndArmingAfterwardsIsInert )
{
    TimerThread timers;
    const auto  before_stop = timers.arm( Clock::now() + stopDeadline );
    timers.stop();
    const auto after_stop = timers.arm( Clock::now() + stopDeadline );

    // Tokens are never reused, even across stop().
    EXPECT_NE( before_stop, after_stop );

    std::this_thread::sleep_for( postStopSpan );
    EXPECT_TRUE( timers.drain().empty() );

    timers.stop();    // idempotent
}

TEST( TimerThreadTiming,
      DestructionWithArmedTimersDoesNotHang )
{
    const auto started = Clock::now();
    {
        TimerThread timers;
        for( std::size_t index = firstIndex; index < armedTimerCount; ++index )
        {
            static_cast<void>( timers.arm( Clock::now() + farDeadline ) );
        }
    }
    EXPECT_LT( Clock::now() - started, teardownBudget );
}

TEST( TimerThreadTiming,
      RepeatedLifecyclesLeakNoDescriptors )
{
    // One warm-up cycle first: the first construction can fault in lazily
    // allocated runtime state, which would otherwise read as a leak.
    {
        TimerThread warmup;
        static_cast<void>( warmup.arm( Clock::now() + farDeadline ) );
    }

    const std::size_t before = open_descriptor_count();
    for( std::size_t cycle = firstIndex; cycle < lifecycleCycles; ++cycle )
    {
        TimerThread timers;
        static_cast<void>( timers.arm( Clock::now() + farDeadline ) );
        static_cast<void>( timers.arm( Clock::now() + wakeDeadline ) );
    }
    EXPECT_EQ( open_descriptor_count(), before );
}
