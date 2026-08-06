#include "kernel/support/diag.hpp"
#include "kernel/support/log.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
// clang-format on

namespace
{

    using namespace std::chrono_literals;

    constexpr std::size_t   noFrames         = 0U;
    constexpr std::size_t   oneFrame         = 1U;
    constexpr std::size_t   sampleCount      = 100U;
    constexpr std::size_t   emptyScopeSize   = 1U;
    constexpr std::uint32_t pixelsPerFrame   = 10U;
    constexpr std::uint32_t eventsPerFrame   = 3U;
    constexpr std::uint32_t serverBaseMs     = 1'000U;
    constexpr std::uint32_t serverStepMs     = 5U;
    constexpr std::uint32_t serverFarFuture  = 4'000'000U;
    constexpr std::uint32_t serverNearWrap   = 0XFF'FF'FF'F0U;
    constexpr std::uint32_t serverAfterWrap  = 4U;
    constexpr auto          wrapDeltaMs      = 20U;
    constexpr auto          overBudgetPhase  = 20ms;
    constexpr auto          withinBudget     = 1ms;
    constexpr std::size_t   noOverruns       = 0U;
    constexpr std::size_t   oneOverrun       = 1U;
    constexpr auto          calibrationSlack = 50ms;

    [[nodiscard]]
    grab::diag::FrameSample
    sample_with( std::chrono::nanoseconds raster )
    {
        return grab::diag::FrameSample{
            .raster           = raster,
            .convert          = {},
            .present          = {},
            .flush            = {},
            .input_to_present = {},
            .damaged_pixels   = pixelsPerFrame,
            .events_drained   = eventsPerFrame,
        };
    }

}    // namespace

// A phase timer whose level is compiled out must be an empty object with no
// clock reads at all — that is what lets the instrument sit inside present_tick
// without costing anything in a release build.
//
// Written as a short-circuiting constant expression rather than a
// `static_assert` inside an `if constexpr`: in a non-template function the
// discarded branch is still fully checked, so the assert would fire in any
// build whose ceiling admits the level. This form is checked for real in the
// `release` preset (ceiling off) and is vacuously true elsewhere.
TEST( Diag,
      ScopeIsEmptyWhenItsLevelIsCompiledOut )
{
    static_assert( grab::log::enabled( grab::log::Level::Debug ) ||
                   sizeof( grab::diag::Scope<grab::log::Level::Debug> ) ==
                   emptyScopeSize );
    static_assert( grab::log::enabled( grab::log::Level::Verbose ) ||
                   sizeof( grab::diag::Scope<grab::log::Level::Verbose> ) ==
                   emptyScopeSize );
    static_assert( grab::log::enabled( grab::log::Level::Nominal ) ||
                   sizeof( grab::diag::Scope<grab::log::Level::Nominal> ) ==
                   emptyScopeSize );
    SUCCEED();
}

TEST( Diag,
      ScopeMeasuresElapsedTimeWhenEnabled )
{
    grab::diag::Scope<grab::log::Level::Nominal> scope;
    std::this_thread::sleep_for( withinBudget );
    const auto elapsed = scope.elapsed();

    if constexpr( grab::log::enabled( grab::log::Level::Nominal ) )
    {
        EXPECT_GT( elapsed, std::chrono::nanoseconds::zero() );
    }
    else
    {
        EXPECT_EQ( elapsed, std::chrono::nanoseconds::zero() );
    }
}

TEST( Diag,
      ReportIsEmptyAfterReset )
{
    grab::diag::reset();
    const auto summary = grab::diag::report();

    EXPECT_EQ( summary.frames, noFrames );
    EXPECT_EQ( summary.total, noFrames );
    EXPECT_EQ( summary.over_budget, noOverruns );
}

TEST( Diag,
      QuantilesSummariseTheRecordedWindow )
{
    grab::diag::reset();
    for( std::size_t index = 0; index < sampleCount; ++index )
    {
        grab::diag::record_frame(
            sample_with( std::chrono::nanoseconds{ static_cast<long>( index ) } )
        );
    }

    const auto summary = grab::diag::report();

    EXPECT_EQ( summary.frames, sampleCount );
    EXPECT_EQ( summary.total, sampleCount );
    EXPECT_EQ( summary.raster.min, std::chrono::nanoseconds::zero() );
    EXPECT_EQ( summary.raster.max, std::chrono::nanoseconds{ sampleCount - 1 } );
    EXPECT_GT( summary.raster.p95, summary.raster.p50 );
    EXPECT_GE( summary.raster.p50, summary.raster.min );
    EXPECT_LE( summary.raster.p95, summary.raster.max );

    EXPECT_EQ( summary.damaged_pixels, pixelsPerFrame * sampleCount );
    EXPECT_EQ( summary.events_drained, eventsPerFrame * sampleCount );

    grab::diag::reset();
}

// The whole point of the instrument: identifying frames that blew the 16.7 ms
// pacing budget.
TEST( Diag,
      CountsFramesThatExceedThePacingBudget )
{
    grab::diag::reset();
    grab::diag::record_frame( sample_with( withinBudget ) );
    EXPECT_EQ( grab::diag::report().over_budget, noOverruns );

    grab::diag::record_frame( sample_with( overBudgetPhase ) );
    EXPECT_EQ( grab::diag::report().over_budget, oneOverrun );

    grab::diag::reset();
}

TEST( Diag,
      WindowRetainsOnlyTheMostRecentFrames )
{
    grab::diag::reset();
    for( std::size_t index = 0; index < grab::diag::frameWindow + oneFrame; ++index )
    {
        grab::diag::record_frame( sample_with( withinBudget ) );
    }

    const auto summary = grab::diag::report();
    EXPECT_EQ( summary.frames, grab::diag::frameWindow );
    EXPECT_EQ( summary.total, grab::diag::frameWindow + oneFrame );

    grab::diag::reset();
}

TEST( Diag,
      ReportIsDueOnTheConfiguredCadence )
{
    grab::diag::reset();
    for( std::size_t index = 0; index < grab::diag::reportCadence - oneFrame; ++index )
    {
        grab::diag::record_frame( sample_with( withinBudget ) );
        EXPECT_FALSE( grab::diag::due_for_report() );
    }
    grab::diag::record_frame( sample_with( withinBudget ) );
    EXPECT_TRUE( grab::diag::due_for_report() );

    grab::diag::reset();
}

// ── ServerClock ────────────────────────────────────────────
//
// An X server timestamp is milliseconds since the server started, in a 32-bit
// counter. It shares no origin with steady_clock, so these cases exist to stop
// the two being subtracted from each other.

TEST( Diag,
      ServerClockYieldsNothingUntilCalibrated )
{
    const grab::diag::ServerClock clock;

    EXPECT_FALSE( clock.calibrated() );
    EXPECT_FALSE( clock.instant_of( serverBaseMs ).has_value() );
}

TEST( Diag,
      ServerClockMapsAnEventForwardOfTheCalibrationPoint )
{
    grab::diag::ServerClock clock;
    const auto              before = std::chrono::steady_clock::now();
    clock.calibrate( serverBaseMs );

    ASSERT_TRUE( clock.calibrated() );
    const auto instant = clock.instant_of( serverBaseMs + serverStepMs );
    ASSERT_TRUE( instant.has_value() );

    EXPECT_GE( *instant, before + std::chrono::milliseconds{ serverStepMs } );
    EXPECT_LE( *instant,
               before + std::chrono::milliseconds{ serverStepMs } + calibrationSlack );
}

// The counter wraps roughly every 49.7 days. Unsigned subtraction wraps the
// same way, so a forward difference across the wrap must still come out right.
TEST( Diag,
      ServerClockHandlesTheCounterWrapping )
{
    grab::diag::ServerClock clock;
    clock.calibrate( serverNearWrap );

    const auto instant = clock.instant_of( serverAfterWrap );
    ASSERT_TRUE( instant.has_value() );

    const auto base = clock.instant_of( serverNearWrap );
    ASSERT_TRUE( base.has_value() );
    EXPECT_EQ( *instant - *base, std::chrono::milliseconds{ wrapDeltaMs } );
}

// A timestamp implausibly far from the calibration point means the calibration
// is stale. Reporting a latency from it would be a confident wrong number,
// which is worse than reporting none.
TEST( Diag,
      ServerClockRefusesAnImplausiblyDistantTimestamp )
{
    grab::diag::ServerClock clock;
    clock.calibrate( serverBaseMs );

    EXPECT_FALSE( clock.instant_of( serverFarFuture ).has_value() );
}
