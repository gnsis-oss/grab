#include "session/poll_wait.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <chrono>
// clang-format on

namespace
{

    constexpr auto shortTimeout  = std::chrono::milliseconds{ 200 };
    constexpr auto shortInterval = std::chrono::milliseconds{ 5 };

}    // namespace

TEST( PollWait,
      ReadyOnTheFirstAskCostsNoSleep )
{
    int        asked   = 0;
    const auto start   = std::chrono::steady_clock::now();
    const auto outcome = grab::session::poll_until(
        [&asked]
        {
            ++asked;
            return grab::session::Probe::Ready;
        },
        shortTimeout,
        shortInterval
    );
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ( outcome, grab::session::Probe::Ready );
    EXPECT_EQ( asked, 1 );
    EXPECT_LT( elapsed, shortInterval );
}

// A zero timeout still asks once: a caller polling a precondition that is
// already satisfied should not have to pay a sleep to hear so.
TEST( PollWait,
      ZeroTimeoutStillAsksOnce )
{
    int        asked   = 0;
    const auto outcome = grab::session::poll_until(
        [&asked]
        {
            ++asked;
            return grab::session::Probe::Retry;
        },
        std::chrono::milliseconds::zero(),
        shortInterval
    );

    EXPECT_EQ( outcome, grab::session::Probe::Retry );
    EXPECT_EQ( asked, 1 );
}

TEST( PollWait,
      RetriesUntilReady )
{
    int        asked   = 0;
    const auto outcome = grab::session::poll_until(
        [&asked]
        {
            ++asked;
            return asked < 3 ? grab::session::Probe::Retry : grab::session::Probe::Ready;
        },
        shortTimeout,
        shortInterval
    );

    EXPECT_EQ( outcome, grab::session::Probe::Ready );
    EXPECT_EQ( asked, 3 );
}

// Abandoned is how "the service died" is told apart from "not yet": the
// difference is a diagnosis, and waiting out the timeout on a corpse loses it.
TEST( PollWait,
      AbandonedStopsImmediately )
{
    int        asked   = 0;
    const auto start   = std::chrono::steady_clock::now();
    const auto outcome = grab::session::poll_until(
        [&asked]
        {
            ++asked;
            return grab::session::Probe::Abandoned;
        },
        std::chrono::seconds{ 30 },
        shortInterval
    );
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ( outcome, grab::session::Probe::Abandoned );
    EXPECT_EQ( asked, 1 );
    EXPECT_LT( elapsed, shortTimeout );
}

TEST( PollWait,
      TimesOutWithRetry )
{
    const auto start   = std::chrono::steady_clock::now();
    const auto outcome = grab::session::poll_until(
        []
        {
            return grab::session::Probe::Retry;
        },
        shortTimeout,
        shortInterval
    );
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ( outcome, grab::session::Probe::Retry );
    EXPECT_GE( elapsed, shortTimeout );
}
