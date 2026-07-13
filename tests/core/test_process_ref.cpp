#include "grab/process_ref.hpp"
#include "grab/result.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <chrono>
#include <sys/wait.h>
#include <type_traits>
#include <unistd.h>
// clang-format on

namespace
{

    constexpr auto terminateGrace = std::chrono::milliseconds{ 500 };

}    // namespace

static_assert( !std::is_constructible_v<grab::OwnedProcess,
                                        grab::BorrowedProcessId> );
// There is deliberately no BorrowedProcessId -> OwnedProcess conversion
// (canonical architecture spec section 3.6).

TEST( ProcessRef,
      AdoptAndTerminateOwnChild )
{
    const auto pid = fork();
    ASSERT_NE( pid, -1 );
    if( pid == 0 )
    {
        const int pause_result = pause();
        static_cast<void>( pause_result );
        _exit( 0 );
    }
    auto owned = grab::OwnedProcess::adopt_child( pid );
    ASSERT_TRUE( owned.has_value() );
    EXPECT_TRUE( owned->alive() );
    EXPECT_TRUE( owned->terminate( terminateGrace ).has_value() );
    EXPECT_FALSE( owned->alive() );
}

TEST( ProcessRef,
      AdoptingDeadPidFails )
{
    const auto pid = fork();
    ASSERT_NE( pid, -1 );
    if( pid == 0 )
    {
        _exit( 0 );
    }
    int status{};
    ASSERT_EQ( waitpid( pid, &status, 0 ),
               pid );    // child fully reaped -> pid invalid
    const auto owned = grab::OwnedProcess::adopt_child( pid );
    EXPECT_FALSE( owned.has_value() );
    EXPECT_EQ( owned.error().code, grab::ErrorCode::OwnershipRequired );
}

TEST( ProcessRef,
      AdoptingNonChildPidIsRejected )
{
    // PID 1 (init) is live and visible but is not our child.
    const auto owned = grab::OwnedProcess::adopt_child( 1 );
    EXPECT_FALSE( owned.has_value() );
    EXPECT_EQ( owned.error().code, grab::ErrorCode::OwnershipRequired );
}
