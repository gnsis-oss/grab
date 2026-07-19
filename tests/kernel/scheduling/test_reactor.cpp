// clang-format off
#include "kernel/scheduling/reactor.hpp"

#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <sys/epoll.h>
#include <thread>
#include <unistd.h>
// clang-format on

namespace
{

    constexpr auto          fenceTimeout = std::chrono::seconds{ 5 };
    constexpr std::uint32_t inEvents     = EPOLLIN;
    constexpr int           posixSuccess = 0;
    constexpr std::size_t   pipeFdCount  = 2U;

}    // namespace

// A client can close its fd between enqueueing add_fd and the reactor
// thread draining the deferred registration (WindowTracker::start
// immediately followed by stop() does exactly this). The racing client
// loses its one registration; the shared loop must keep serving everyone
// else instead of exiting on the EBADF.
TEST( Reactor,
      SurvivesDeferredAddForClosedFd )
{
    grab::core::Reactor          reactor;

    std::array<int, pipeFdCount> pipe_fds{ -1, -1 };
    ASSERT_EQ( ::pipe( pipe_fds.data() ), posixSuccess );
    ASSERT_EQ( ::close( pipe_fds.at( 0U ) ), posixSuccess );
    ASSERT_EQ( ::close( pipe_fds.at( 1U ) ), posixSuccess );

    const auto token = reactor.add_fd( pipe_fds.at( 0U ),
                                       inEvents,
                                       []( std::uint32_t )
                                       {
                                       } );
    static_cast<void>( token );

    grab::Result<void> run_result;
    std::promise<void> fence;
    auto               reached = fence.get_future();

    std::thread        runner(
        [&reactor, &run_result]
        {
            run_result = reactor.run();
        }
    );
    reactor.post(
        [&fence]
        {
            fence.set_value();
        }
    );

    const auto status = reached.wait_for( fenceTimeout );
    reactor.stop();
    runner.join();

    EXPECT_EQ( status, std::future_status::ready );
    EXPECT_TRUE( run_result.has_value() );
}
