#include "grab/process_ref.hpp"
#include "grab/result.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <climits>
#include <string>
#include <string_view>
#include <unistd.h>
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

namespace
{

    // Where the child's stdout actually pointed, read back from the child
    // itself: nothing else can observe a redirect that happens between clone
    // and exec.
    [[nodiscard]]
    std::string
    child_stdout_target( bool discard_output )
    {
        const auto report =
            std::filesystem::temp_directory_path() /
            ( discard_output ? "grab-spawn-discarded" : "grab-spawn-inherited" );
        std::filesystem::remove( report );

        // The shell's own fd 1 is what is under test, so it is read before
        // the descriptor that carries the answer back is opened: a redirect
        // written into the script would answer about the redirect.
        const std::string script = "target=$(readlink /proc/$$/fd/1); exec 9>" +
                                   report.string() +
                                   "; printf '%s' \"$target\" >&9";
        const std::array<std::string_view, 3U> argv{ "/bin/sh", "-c", script };

        auto process = grab::OwnedProcess::spawn( argv,
                                                  {},
                                                  grab::ProcessSpawnOptions{
                                                      .search_path    = false,
                                                      .discard_output = discard_output,
                                                  } );
        if( !process.has_value() )
        {
            return process.error().message;
        }
        const auto status = process->wait( waitTimeout );
        if( !status.has_value() )
        {
            return status.error().message;
        }

        std::ifstream input{ report };
        std::string   target;
        std::getline( input, target );
        std::filesystem::remove( report );
        return target;
    }

}    // namespace

// A spawned service — a window manager, a bus, a compositor — is chatty, and
// its chatter is not its caller's report.
TEST( ProcessSpawn,
      DiscardOutputPointsTheChildAtDevNull )
{
    EXPECT_EQ( child_stdout_target( true ), "/dev/null" );
}

TEST( ProcessSpawn,
      OutputIsInheritedByDefault )
{
    // Whatever this process's stdout is — a terminal, a pipe under ctest, a
    // file — the child gets that same one and nothing is redirected.
    std::array<char, PATH_MAX> own{};
    const auto length = ::readlink( "/proc/self/fd/1", own.data(), own.size() - 1U );
    ASSERT_GT( length, 0 );

    EXPECT_EQ( child_stdout_target( false ),
               std::string( own.data(), static_cast<std::size_t>( length ) ) );
}
