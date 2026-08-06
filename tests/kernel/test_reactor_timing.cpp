// Reactor deadline precision.
//
// The suite is named ReactorTiming because tests/CMakeLists.txt labels that
// suite "timing", so a loaded machine can exclude it with `ctest -LE timing`.
// Every assertion here is a tolerance against a shared box, so the budgets are
// loose in the direction noise pushes and tight only where a regression would
// be structural rather than noisy.
//
// The regression being guarded is `epoll_wait`'s millisecond timeout:
// `ceil<milliseconds>` turned a 200 us delay into 1 ms, so a sub-millisecond
// deadline was simply unrepresentable. With a timerfd carrying the deadline,
// the *best* of a handful of attempts must land well inside a millisecond.
// Under the old code every single attempt was >= 1 ms, which makes a best-of-N
// both a sharp discriminator and immune to one descheduled sample.
//
// Shutdown discipline: every test waits on its fences, then stops and joins the
// reactor, and only then asserts. A GTest ASSERT_* that fires while the loop is
// still running would return from the test body and destroy the locals its
// callbacks capture by reference.

#include "grab/result.hpp"
#include "kernel/scheduling/reactor.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <thread>
#include <vector>
// clang-format on

namespace
{

    using Clock    = std::chrono::steady_clock;
    using Duration = Clock::duration;

    // The deadline under test: sub-millisecond, so the old ceil-to-ms path
    // could not represent it.
    constexpr auto        shortDelay = std::chrono::microseconds{ 200 };

    // Best-of-N budget. 200 us of deadline plus the enqueue/wake round trip
    // and one epoll dispatch; 900 us leaves ~700 us of slack while still
    // sitting below the 1 ms floor the old implementation could never beat.
    constexpr auto        shortDelayBudget = std::chrono::microseconds{ 900 };

    // Enough attempts that a single descheduled sample cannot fail the run,
    // few enough that the suite stays well under a second.
    constexpr std::size_t shortDelaySamples = 9U;

    // Ordering probe: three timers queued nearest-last, so the reactor has to
    // rearm at a deadline closer than the one already loaded into the fd.
    constexpr auto        farDelay    = std::chrono::milliseconds{ 12 };
    constexpr auto        middleDelay = std::chrono::milliseconds{ 8 };
    constexpr auto        nearDelay   = std::chrono::milliseconds{ 4 };

    constexpr int         farMark     = 3;
    constexpr int         middleMark  = 2;
    constexpr int         nearMark    = 1;

    // Chained rearm. `add_timer` takes a RELATIVE delay and stamps the
    // deadline at drain time, so each link re-bases on the previous link's
    // overshoot and the error compounds. The absolute timerfd arm removes the
    // ceil-to-millisecond and the per-arm rounding; it does NOT remove this
    // drift, and a tight total here would be asserting a promise the API never
    // made. The budget is a sanity ceiling on accumulated drift — what it
    // really proves is that the loop keeps rearming and never wedges in an
    // unarmed infinite wait.
    //
    // 20 links of 200 us is 4 ms of deadline; the ceiling is 60x that because
    // each link pays a separate thread wake on a machine CLAUDE.md warns can
    // be 50% off on a single run, and 20 of those compound. A wedge shows up
    // as the 5 s fence, not as a near miss here.
    constexpr std::size_t chainLength      = 20U;
    constexpr auto        chainLinkDelay   = std::chrono::microseconds{ 200 };
    constexpr auto        chainDriftBudget = std::chrono::milliseconds{ 240 };

    constexpr auto        fenceTimeout     = std::chrono::seconds{ 5 };
    constexpr auto        settleTimeout    = std::chrono::seconds{ 5 };
    constexpr std::size_t firstSample      = 0U;
    constexpr std::size_t noneRemaining    = 0U;

    // Runs a reactor on its own thread and guarantees the loop is already
    // inside `epoll_wait` before any measurement starts, so no sample pays for
    // thread creation. Declare it FIRST in a test: it is then destroyed last,
    // and `shutdown()` is idempotent.
    class RunningReactor
    {
        public:

            RunningReactor() :
                runner_(
                    [this]
                    {
                        run_result_ = reactor_.run();
                    }
                )
            {
                std::promise<void> ready;
                auto               reached = ready.get_future();
                reactor_.post(
                    [&ready]
                    {
                        ready.set_value();
                    }
                );
                running_ =
                    reached.wait_for( settleTimeout ) == std::future_status::ready;
            }

            ~RunningReactor()
            {
                shutdown();
            }

            RunningReactor( const RunningReactor& ) = delete;
            RunningReactor&
            operator=( const RunningReactor& ) = delete;
            RunningReactor( RunningReactor&& ) = delete;
            RunningReactor&
            operator=( RunningReactor&& ) = delete;

            void
            shutdown() noexcept
            {
                if( !runner_.joinable() )
                {
                    return;
                }
                reactor_.stop();
                runner_.join();
            }

            [[nodiscard]]
            bool
            running() const noexcept
            {
                return running_;
            }

            // Only meaningful after `shutdown()`.
            [[nodiscard]]
            bool
            exited_cleanly() const noexcept
            {
                return run_result_.has_value();
            }

            [[nodiscard]]
            grab::core::Reactor&
            reactor() noexcept
            {
                return reactor_;
            }

        private:

            grab::core::Reactor reactor_;
            grab::Result<void>  run_result_;
            std::thread         runner_;
            bool                running_ = false;
    };

    // One measured firing: wall time from just before `add_timer` to the
    // callback body, which includes the enqueue/wake round trip a real caller
    // also waits for. The promise is heap-owned so a timed-out firing cannot
    // leave the reactor writing into a dead stack frame.
    [[nodiscard]]
    Duration
    measure_one( grab::core::Reactor&     reactor,
                 std::chrono::nanoseconds delay )
    {
        auto       fired    = std::make_shared<std::promise<Duration>>();
        auto       observed = fired->get_future();

        const auto issued   = Clock::now();
        const auto token =
            reactor.add_timer( delay,
                               [fired, issued]
                               {
                                   fired->set_value( Clock::now() - issued );
                               } );
        static_cast<void>( token );

        if( observed.wait_for( fenceTimeout ) != std::future_status::ready )
        {
            return Duration::max();
        }
        return observed.get();
    }

    // Every latency assertion here is best-of-N, never single-sample. A lone
    // firing measures the box's thread-wake latency as much as the reactor's:
    // an early single-sample version of the zero-delay test read 3.7 ms on a
    // machine at load 35 and failed, with nothing wrong in the reactor. The
    // *minimum* over a handful of attempts is the reactor's own floor, and it
    // is what the millisecond regression would lift above budget.
    [[nodiscard]]
    std::vector<Duration>
    sample_latencies( grab::core::Reactor&     reactor,
                      std::chrono::nanoseconds delay )
    {
        std::vector<Duration> samples;
        samples.reserve( shortDelaySamples );
        for( std::size_t attempt = firstSample; attempt < shortDelaySamples; ++attempt )
        {
            samples.push_back( measure_one( reactor, delay ) );
        }
        return samples;
    }

}    // namespace

// The core regression. A 200 us deadline must be reachable; under the
// millisecond timeout it was not, because `ceil<milliseconds>(200us)` is 1 ms
// and `epoll_wait` guarantees only *at least* its timeout.
TEST( ReactorTiming,
      SubMillisecondDelayIsNotRoundedUpToAMillisecond )
{
    RunningReactor        host;

    std::vector<Duration> samples;
    if( host.running() )
    {
        samples = sample_latencies( host.reactor(), shortDelay );
    }
    host.shutdown();

    ASSERT_TRUE( host.running() );
    ASSERT_EQ( samples.size(), shortDelaySamples );

    // Never early: a deadline the reactor beats is as wrong as one it misses.
    for( const auto& sample : samples )
    {
        EXPECT_GE( sample, shortDelay );
    }

    const auto best = *std::ranges::min_element( samples );
    EXPECT_LT( best, shortDelayBudget )
        << "best of " << shortDelaySamples << " firings of a "
        << std::chrono::duration_cast<std::chrono::microseconds>( shortDelay ).count()
        << " us delay took "
        << std::chrono::duration_cast<std::chrono::microseconds>( best ).count()
        << " us; >= 1000 us means the millisecond epoll timeout is back";
    EXPECT_TRUE( host.exited_cleanly() );
}

// A zero delay must not wait for a tick boundary. This pins the timerfd arm
// against its own failure mode: an absolute deadline already in the past has
// to expire immediately rather than land on the all-zero `it_value` that
// `timerfd_settime` reads as *disarm*, which would block the loop forever.
TEST( ReactorTiming,
      ZeroDelayFiresWithoutWaiting )
{
    RunningReactor        host;

    std::vector<Duration> samples;
    if( host.running() )
    {
        samples = sample_latencies( host.reactor(), std::chrono::nanoseconds::zero() );
    }
    host.shutdown();

    ASSERT_TRUE( host.running() );
    ASSERT_EQ( samples.size(), shortDelaySamples );

    const auto best = *std::ranges::min_element( samples );
    EXPECT_LT( best, shortDelayBudget )
        << "best of " << shortDelaySamples << " zero-delay firings took "
        << std::chrono::duration_cast<std::chrono::microseconds>( best ).count()
        << " us";
    EXPECT_TRUE( host.exited_cleanly() );
}

// The timerfd is armed from `timers_.front()`, the minimum of a binary heap.
// Queueing the nearest deadline last forces a rearm at a time closer than the
// one already loaded into the fd; without that rearm the near timer would be
// held back until the far one expired.
TEST( ReactorTiming,
      NearerDeadlineAddedLastStillFiresFirst )
{
    RunningReactor     host;

    std::promise<void> far_fired;
    std::promise<void> middle_fired;
    std::promise<void> near_fired;

    auto               far_done    = far_fired.get_future();
    auto               middle_done = middle_fired.get_future();
    auto               near_done   = near_fired.get_future();

    // Written only from the reactor thread, which runs callbacks serially, and
    // read only after the join below.
    std::vector<int>   order;

    auto               far_status    = std::future_status::timeout;
    auto               middle_status = std::future_status::timeout;
    auto               near_status   = std::future_status::timeout;

    if( host.running() )
    {
        static_cast<void>( host.reactor().add_timer( farDelay,
                                                     [&order, &far_fired]
                                                     {
                                                         order.push_back( farMark );
                                                         far_fired.set_value();
                                                     } ) );
        static_cast<void>( host.reactor().add_timer( middleDelay,
                                                     [&order, &middle_fired]
                                                     {
                                                         order.push_back( middleMark );
                                                         middle_fired.set_value();
                                                     } ) );
        static_cast<void>( host.reactor().add_timer( nearDelay,
                                                     [&order, &near_fired]
                                                     {
                                                         order.push_back( nearMark );
                                                         near_fired.set_value();
                                                     } ) );

        near_status   = near_done.wait_for( fenceTimeout );
        middle_status = middle_done.wait_for( fenceTimeout );
        far_status    = far_done.wait_for( fenceTimeout );
    }
    host.shutdown();

    ASSERT_TRUE( host.running() );
    ASSERT_EQ( near_status, std::future_status::ready );
    ASSERT_EQ( middle_status, std::future_status::ready );
    ASSERT_EQ( far_status, std::future_status::ready );

    const std::vector<int> expected{ nearMark, middleMark, farMark };
    EXPECT_EQ( order, expected );
    EXPECT_TRUE( host.exited_cleanly() );
}

// Rearming from inside a callback, 20 links of 200 us. The assertion is a
// generous ceiling rather than a precision claim — see `chainDriftBudget`.
TEST( ReactorTiming,
      ChainedRearmsCompleteWithinADriftCeiling )
{
    RunningReactor        host;

    std::promise<void>    chain_done;
    auto                  finished  = chain_done.get_future();
    std::size_t           remaining = chainLength;
    std::function<void()> link;

    auto                  chain_status = std::future_status::timeout;
    auto                  elapsed      = Duration::max();

    if( host.running() )
    {
        link = [&host, &link, &remaining, &chain_done]
        {
            --remaining;
            if( remaining == noneRemaining )
            {
                chain_done.set_value();
                return;
            }
            static_cast<void>( host.reactor().add_timer( chainLinkDelay, link ) );
        };

        const auto issued = Clock::now();
        static_cast<void>( host.reactor().add_timer( chainLinkDelay, link ) );
        chain_status = finished.wait_for( fenceTimeout );
        elapsed      = Clock::now() - issued;
    }
    host.shutdown();

    ASSERT_TRUE( host.running() );
    ASSERT_EQ( chain_status, std::future_status::ready );
    EXPECT_EQ( remaining, noneRemaining );

    EXPECT_GE( elapsed, chainLinkDelay )
        << "the chain cannot finish before its first link is due";
    EXPECT_LT(
        elapsed,
        chainDriftBudget
    ) << chainLength
      << " chained "
      << std::chrono::duration_cast<std::chrono::microseconds>( chainLinkDelay ).count()
      << " us rearms took "
      << std::chrono::duration_cast<std::chrono::microseconds>( elapsed ).count()
      << " us";
    EXPECT_TRUE( host.exited_cleanly() );
}
