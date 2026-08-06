// Drift<N, Strategy> and the ServerClock built on it.
//
// Two things are under test here and they fail differently. The estimator
// fails numerically — a strategy that reads its own ring in the wrong order,
// or a mean quietly following an outlier. The clock fails silently: a
// correlation that stops answering once a session outlives its calibration
// window returns nullopt forever, and the only symptom is a latency figure
// that reads zero. The 90-second cases exist for exactly that failure, which
// no short test can see.

#include "kernel/support/diag.hpp"
#include "kernel/support/drift.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
// clang-format on

namespace
{

    using Offset = std::chrono::nanoseconds;

    // Small enough to wrap by hand in a test, large enough that a median has
    // something to reject.
    constexpr std::size_t driftCapacity = 8U;

    template<typename Strategy>
    using Estimator                   = grab::core::Drift<driftCapacity, Strategy>;

    constexpr std::size_t   noSamples = 0U;
    constexpr std::size_t   oneSample = 1U;

    // A plausible offset between two clocks, and the jitter around it that a
    // scheduler contributes to every sample.
    constexpr Offset        typicalOffset{ 1'000'000 };    // 1 ms
    constexpr Offset        jitterOffset{ 100'000 };       // 0.1 ms

    // A single preempted sample: half a second of scheduling delay landing in
    // one observation. This is what a single-point calibration cannot survive.
    constexpr Offset        outlierOffset{ 500'000'000 };    // 500 ms

    constexpr Offset        staleOffset{ 500'000'000 };
    constexpr Offset        freshOffset{ 1'000'000 };

    // The mean must be dragged past this multiple of the typical offset for
    // the median's rejection of the same series to mean anything.
    constexpr Offset::rep   meanOutlierFactor = 2;

    // A rising series long enough to wrap the ring twice and stop two slots
    // short of a third wrap, so storage order and insertion order disagree.
    constexpr std::size_t   rampLength = ( 3U * driftCapacity ) + 2U;
    constexpr Offset::rep   rampStep   = 1'000'000;

    // The EMA is computed in double and rounded; two independent evaluations
    // of the same recurrence may differ in the last bit.
    constexpr double        emaTolerance = 8.0;

    // ── ServerClock ────────────────────────────────────────

    constexpr std::uint32_t serverOriginMs = 1'000'000U;

    // Longer than the 60 s staleness window, which is the point: a session
    // this old is what freezes a single-point calibration permanently.
    constexpr std::uint32_t sessionSpanMs = 90'000U;
    constexpr auto          sessionSpan   = std::chrono::milliseconds{ sessionSpanMs };

    constexpr std::uint32_t calibrationIntervalMs = 100U;
    constexpr auto          calibrationInterval =
        std::chrono::milliseconds{ calibrationIntervalMs };
    constexpr std::uint32_t calibrationSteps = sessionSpanMs / calibrationIntervalMs;

    // One calibration in the final window arrives this late.
    constexpr auto          outlierLatency      = std::chrono::milliseconds{ 250 };
    constexpr std::uint32_t outlierStepsFromEnd = 3U;

    // Slack for the wall-clock instants the simulation is anchored to: the
    // arithmetic is exact, but `now` keeps moving while the case runs. Far
    // below outlierLatency, so a rejected outlier is still distinguishable
    // from an accepted one.
    constexpr auto          resolutionTolerance = std::chrono::milliseconds{ 25 };

    // Reference EMA over a series in the order it was fed. Deliberately a
    // separate implementation from the one under test: an oracle that shares
    // the ring's indexing would share its bugs.
    [[nodiscard]]
    double
    ema_of( const std::array<Offset,
                             driftCapacity>& series ) noexcept
    {
        constexpr double alpha = 2.0 / ( static_cast<double>( driftCapacity ) + 1.0 );

        double           value = 0.0;
        for( std::size_t index = 0; index < series.size(); ++index )
        {
            const double sample = static_cast<double>( series[index].count() );
            value =
                index == 0 ? sample : ( alpha * sample ) + ( ( 1.0 - alpha ) * value );
        }
        return value;
    }

    [[nodiscard]]
    Offset
    absolute( Offset value ) noexcept
    {
        return value < Offset::zero() ? -value : value;
    }

    [[nodiscard]]
    Offset
    distance( std::chrono::steady_clock::time_point left,
              std::chrono::steady_clock::time_point right ) noexcept
    {
        return absolute( std::chrono::duration_cast<Offset>( left - right ) );
    }

}    // namespace

// ── Drift ──────────────────────────────────────────────────

TEST( Drift,
      EmptyEstimatorReportsNoSamplesAndNoShift )
{
    static_assert( Estimator<grab::core::drift::Median>::capacity == driftCapacity );

    const Estimator<grab::core::drift::Mean>   mean;
    const Estimator<grab::core::drift::Median> median;
    const Estimator<grab::core::drift::Ema>    ema;

    EXPECT_EQ( mean.samples(), noSamples );
    EXPECT_EQ( median.samples(), noSamples );
    EXPECT_EQ( ema.samples(), noSamples );

    EXPECT_EQ( mean.shift(), Offset::zero() );
    EXPECT_EQ( median.shift(), Offset::zero() );
    EXPECT_EQ( ema.shift(), Offset::zero() );

    EXPECT_EQ( mean.spread(), Offset::zero() );
    EXPECT_EQ( median.spread(), Offset::zero() );
    EXPECT_EQ( ema.spread(), Offset::zero() );
}

// The reason Median is the default. One preempted sample moves the mean by
// most of its own size; it moves the median by nothing, because it changes
// which sample sits in the middle rather than what the middle is.
TEST( Drift,
      MedianRejectsAnOutlierThatDragsTheMean )
{
    Estimator<grab::core::drift::Mean>   mean;
    Estimator<grab::core::drift::Median> median;

    for( std::size_t index = 0; index + oneSample < driftCapacity; ++index )
    {
        // -1, 0, +1 jitter steps around the typical offset.
        const auto step   = static_cast<Offset::rep>( index % 3U ) - 1;
        const auto sample = typicalOffset + ( jitterOffset * step );
        mean.feed( sample );
        median.feed( sample );
    }
    mean.feed( outlierOffset );
    median.feed( outlierOffset );

    ASSERT_EQ( mean.samples(), driftCapacity );
    ASSERT_EQ( median.samples(), driftCapacity );

    EXPECT_GT( mean.shift(), typicalOffset * meanOutlierFactor );
    EXPECT_LE( absolute( median.shift() - typicalOffset ), jitterOffset );

    // The outlier is not in the estimate, but it is in the error bar.
    EXPECT_GT( median.spread(), jitterOffset );
}

// An EMA is order-sensitive, so it has to walk the ring in *insertion* order.
// After a wrap those two orders differ, and a naive index loop weights the
// oldest sample most heavily — the exact inversion of what an EMA is for.
// The case is built so the two orders provably disagree before the estimator
// is asked anything.
TEST( Drift,
      EmaFollowsInsertionOrderAcrossARingWrap )
{
    std::array<Offset, rampLength> fed{};
    for( std::size_t index = 0; index < rampLength; ++index )
    {
        fed[index] = Offset{ rampStep * static_cast<Offset::rep>( index + 1U ) };
    }

    // The window the ring retains, in the order it was fed.
    std::array<Offset, driftCapacity> insertion{};
    for( std::size_t index = 0; index < driftCapacity; ++index )
    {
        insertion[index] = fed[rampLength - driftCapacity + index];
    }

    // The same samples in the order they sit in storage: the slot for the
    // sample fed at position p is p % N, so the window is rotated by the
    // write cursor.
    std::array<Offset, driftCapacity> storage{};
    const std::size_t                 cursor = rampLength % driftCapacity;
    for( std::size_t index = 0; index < driftCapacity; ++index )
    {
        storage[( cursor + index ) % driftCapacity] = insertion[index];
    }

    const double insertion_ema = ema_of( insertion );
    const double storage_ema   = ema_of( storage );
    ASSERT_GT( std::abs( insertion_ema - storage_ema ), emaTolerance )
        << "the two orders must disagree or this case proves nothing";

    Estimator<grab::core::drift::Ema> ema;
    for( const auto sample : fed )
    {
        ema.feed( sample );
    }

    ASSERT_EQ( ema.samples(), driftCapacity );
    EXPECT_NEAR( static_cast<double>( ema.shift().count() ),
                 insertion_ema,
                 emaTolerance );
}

TEST( Drift,
      WindowForgetsSamplesOlderThanItsCapacity )
{
    Estimator<grab::core::drift::Median> median;

    for( std::size_t index = 0; index < driftCapacity; ++index )
    {
        median.feed( staleOffset );
    }
    ASSERT_EQ( median.samples(), driftCapacity );
    ASSERT_EQ( median.shift(), staleOffset );

    for( std::size_t index = 0; index < driftCapacity; ++index )
    {
        median.feed( freshOffset );
    }

    EXPECT_EQ( median.samples(), driftCapacity );
    EXPECT_EQ( median.shift(), freshOffset );
    EXPECT_EQ( median.spread(), Offset::zero() );
}

// spread() is the standard deviation of the window whatever the strategy —
// the error bar that turns an estimate into an honest one.
TEST( Drift,
      SpreadIsTheStandardDeviationOfTheWindow )
{
    Estimator<grab::core::drift::Median> single;
    single.feed( typicalOffset );
    EXPECT_EQ( single.samples(), oneSample );
    EXPECT_EQ( single.spread(), Offset::zero() );

    Estimator<grab::core::drift::Median> constant;
    for( std::size_t index = 0; index < driftCapacity; ++index )
    {
        constant.feed( typicalOffset );
    }
    EXPECT_EQ( constant.spread(), Offset::zero() );

    // Half the window at +jitter and half at -jitter: the population standard
    // deviation of that is exactly the jitter.
    Estimator<grab::core::drift::Median> jittered;
    for( std::size_t index = 0; index < driftCapacity; ++index )
    {
        jittered.feed( index % 2U == 0U ? typicalOffset + jitterOffset
                                        : typicalOffset - jitterOffset );
    }
    EXPECT_EQ( jittered.spread(), jitterOffset );
}

// ── ServerClock ────────────────────────────────────────────

TEST( ServerClockDrift,
      UncalibratedClockHoldsNoSamples )
{
    const grab::diag::ServerClock clock;

    EXPECT_FALSE( clock.calibrated() );
    EXPECT_EQ( clock.samples(), noSamples );
    EXPECT_EQ( clock.spread(), Offset::zero() );
    EXPECT_FALSE( clock.instant_of( serverOriginMs ).has_value() );
}

// The regression case. A session that has been running for 90 seconds is
// older than the 60 s staleness window, so a correlation that measures
// staleness from its calibration point rejects every event it will ever see
// again — silently, and for the rest of the session. Elapsed time is
// simulated by placing the calibration in the past rather than by sleeping.
TEST( ServerClockDrift,
      SingleCalibrationStillResolvesAfterNinetySeconds )
{
    const auto              now    = std::chrono::steady_clock::now();
    const auto              origin = now - sessionSpan;

    grab::diag::ServerClock clock;
    clock.calibrate( serverOriginMs, origin );
    ASSERT_TRUE( clock.calibrated() );
    ASSERT_EQ( clock.samples(), oneSample );

    const auto instant = clock.instant_of( serverOriginMs + sessionSpanMs );
    ASSERT_TRUE( instant.has_value() )
        << "a 90-second-old calibration must still resolve current events";
    EXPECT_LE( distance( *instant, now ), resolutionTolerance );
}

// The same session, calibrated repeatedly the way a live delegate would.
TEST( ServerClockDrift,
      RepeatedCalibrationAcrossNinetySecondsStillResolves )
{
    const auto              now    = std::chrono::steady_clock::now();
    const auto              origin = now - sessionSpan;

    grab::diag::ServerClock clock;
    for( std::uint32_t step = 0; step <= calibrationSteps; ++step )
    {
        clock.calibrate( serverOriginMs + ( step * calibrationIntervalMs ),
                         origin + ( calibrationInterval * step ) );
    }

    EXPECT_EQ( clock.samples(), grab::diag::ServerClock::driftWindow );

    const auto instant = clock.instant_of( serverOriginMs + sessionSpanMs );
    ASSERT_TRUE( instant.has_value() );
    EXPECT_LE( distance( *instant, now ), resolutionTolerance );
}

// What the drift estimate buys over the single calibration point it replaced:
// one preempted sample lands in the window and moves nothing.
TEST( ServerClockDrift,
      OutlyingCalibrationSampleDoesNotMoveTheEstimate )
{
    const auto              now      = std::chrono::steady_clock::now();
    const auto              origin   = now - sessionSpan;
    const auto              poisoned = calibrationSteps - outlierStepsFromEnd;

    grab::diag::ServerClock clock;
    for( std::uint32_t step = 0; step <= calibrationSteps; ++step )
    {
        const auto observed_at =
            origin +
            ( calibrationInterval * step ) +
            ( step == poisoned ? outlierLatency : std::chrono::milliseconds::zero() );
        clock.calibrate( serverOriginMs + ( step * calibrationIntervalMs ),
                         observed_at );
    }

    const auto instant = clock.instant_of( serverOriginMs + sessionSpanMs );
    ASSERT_TRUE( instant.has_value() );

    // A single-point calibration taken on the poisoned sample would be wrong
    // by outlierLatency; the median is wrong by nothing measurable.
    EXPECT_LE( distance( *instant, now ), resolutionTolerance );

    // Rejected from the estimate, still visible in the error bar.
    EXPECT_GT( clock.spread(), Offset::zero() );
}

// Staleness is judged against now, so an event from the start of a long
// session is correctly refused — the useful half of the rule that must not
// also refuse current events.
TEST( ServerClockDrift,
      EventOlderThanTheSkewWindowIsRefused )
{
    const auto              now    = std::chrono::steady_clock::now();
    const auto              origin = now - sessionSpan;

    grab::diag::ServerClock clock;
    for( std::uint32_t step = 0; step <= calibrationSteps; ++step )
    {
        clock.calibrate( serverOriginMs + ( step * calibrationIntervalMs ),
                         origin + ( calibrationInterval * step ) );
    }

    EXPECT_FALSE( clock.instant_of( serverOriginMs ).has_value() );
    EXPECT_TRUE( clock.instant_of( serverOriginMs + sessionSpanMs ).has_value() );
}
