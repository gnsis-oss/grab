#include "grab/result.hpp"
#include "kernel/capture/pacing_governor.hpp"

#include <chrono>
#include <cstdint>
#include <gtest/gtest.h>

TEST( PacingGovernor,
      RejectsZeroFrameRate )
{
    const auto governor = grab::kernel::capture::PacingGovernor::for_fps( 0U );
    ASSERT_FALSE( governor.has_value() );
    EXPECT_EQ( governor.error().code, grab::ErrorCode::InvalidArgument );
}

TEST( PacingGovernor,
      IntervalIsReciprocalOfFrameRate )
{
    const auto governor = grab::kernel::capture::PacingGovernor::for_fps( 10U );
    ASSERT_TRUE( governor.has_value() );
    EXPECT_EQ( governor->interval(), std::chrono::nanoseconds{ 100'000'000 } );
}

TEST( PacingGovernor,
      NextDeadlineAddsOneIntervalToBase )
{
    const auto governor = grab::kernel::capture::PacingGovernor::for_fps( 25U );
    ASSERT_TRUE( governor.has_value() );
    const auto base =
        std::chrono::steady_clock::time_point{} + std::chrono::seconds{ 5 };
    EXPECT_EQ( governor->next_deadline( base ), base + governor->interval() );
}
