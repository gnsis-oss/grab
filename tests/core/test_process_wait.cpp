#include "grab/process_ref.hpp"
#include "grab/result.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <string_view>
// clang-format on

namespace
{

    constexpr auto trueCommand  = std::to_array<std::string_view>( { "/bin/true" } );
    constexpr auto falseCommand = std::to_array<std::string_view>( { "/bin/false" } );
    constexpr auto sleepCommand =
        std::to_array<std::string_view>( { "/bin/sleep", "30" } );

    constexpr auto waitTimeout      = std::chrono::seconds{ 5 };
    constexpr auto shortWaitTimeout = std::chrono::milliseconds{ 50 };
    constexpr auto terminateGrace   = std::chrono::milliseconds{ 500 };
    constexpr int  successStatus    = 0;
    constexpr int  failureStatus    = 1;

}    // namespace

TEST( ProcessWait,
      WaitReturnsExitStatus )
{
    auto successful_process = grab::OwnedProcess::spawn( trueCommand );
    ASSERT_TRUE( successful_process.has_value() ) << successful_process.error().message;

    const auto successful_status = successful_process->wait( waitTimeout );
    ASSERT_TRUE( successful_status.has_value() ) << successful_status.error().message;
    EXPECT_EQ( *successful_status, successStatus );

    auto failing_process = grab::OwnedProcess::spawn( falseCommand );
    ASSERT_TRUE( failing_process.has_value() ) << failing_process.error().message;

    const auto failing_status = failing_process->wait( waitTimeout );
    ASSERT_TRUE( failing_status.has_value() ) << failing_status.error().message;
    EXPECT_EQ( *failing_status, failureStatus );
}

TEST( ProcessWait,
      WaitTimesOutOnRunningChild )
{
    auto process = grab::OwnedProcess::spawn( sleepCommand );
    ASSERT_TRUE( process.has_value() ) << process.error().message;

    const auto timed_out = process->wait( shortWaitTimeout );
    ASSERT_FALSE( timed_out.has_value() );
    EXPECT_EQ( timed_out.error().code, grab::ErrorCode::DeadlineExceeded );

    const auto terminated = process->terminate( terminateGrace );
    ASSERT_TRUE( terminated.has_value() ) << terminated.error().message;

    const auto status = process->wait( waitTimeout );
    EXPECT_TRUE( status.has_value() ) << status.error().message;
}

TEST( ProcessWait,
      WaitOnReapedChildErrors )
{
    auto process = grab::OwnedProcess::spawn( trueCommand );
    ASSERT_TRUE( process.has_value() ) << process.error().message;

    const auto first_wait = process->wait( waitTimeout );
    ASSERT_TRUE( first_wait.has_value() ) << first_wait.error().message;

    const auto second_wait = process->wait( waitTimeout );
    EXPECT_FALSE( second_wait.has_value() );
}
