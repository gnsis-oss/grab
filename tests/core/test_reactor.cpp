#include "core/reactor.hpp"
#include "grab/result.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <sys/epoll.h>    // IWYU pragma: keep
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>
// clang-format on

namespace
{

    constexpr int           kInvalidFd           = -1;
    constexpr int           kPosixSuccess        = 0;
    constexpr unsigned int  kEventfdInitialValue = 0U;
    constexpr eventfd_t     kEventfdSignalValue  = 1U;
    constexpr std::uint64_t kNoToken             = 0U;
    constexpr std::uint32_t kNoEvents            = 0U;
    constexpr int           kNoCallbacks         = 0;
    constexpr int           kOneCallback         = 1;
    constexpr auto          kThreadReadyTimeout  = std::chrono::seconds{ 2 };
    constexpr auto          kCallbackTimeout     = std::chrono::seconds{ 2 };
    constexpr auto          kTimerDelay          = std::chrono::milliseconds{ 20 };
    constexpr auto          kNoCallbackWindow    = std::chrono::milliseconds{ 30 };

    class UniqueFd
    {
        public:

            explicit UniqueFd( int fd ) noexcept :
                fd_( fd )
            {
            }

            ~UniqueFd() noexcept
            {
                reset();
            }

            UniqueFd( const UniqueFd& ) = delete;
            UniqueFd&
            operator=( const UniqueFd& ) = delete;
            UniqueFd( UniqueFd&& )       = delete;
            UniqueFd&
            operator=( UniqueFd&& ) = delete;

            [[nodiscard]]
            int
            get() const noexcept
            {
                return fd_;
            }

        private:

            void
            reset() noexcept
            {
                if( fd_ != kInvalidFd )
                {
                    const auto close_result = ::close( fd_ );
                    static_cast<void>( close_result );
                    fd_ = kInvalidFd;
                }
            }

            int fd_ = kInvalidFd;
    };

}    // namespace

TEST( Reactor,
      ReadableFdFiresCallbackWithReadableEvent )
{
    grab::core::Reactor reactor;
    UniqueFd            event_fd( ::eventfd( kEventfdInitialValue, EFD_CLOEXEC ) );
    ASSERT_NE( event_fd.get(), kInvalidFd );

    std::promise<void>            callback_ran;
    auto                          callback_future = callback_ran.get_future();
    std::atomic<int>              callback_count{ kNoCallbacks };
    std::uint32_t                 seen_events = kNoEvents;
    grab::Result<void>            run_result;
    std::promise<std::thread::id> reactor_started;
    auto                          reactor_started_future = reactor_started.get_future();

    const auto                    token                  = reactor.add_fd(
        event_fd.get(),
        EPOLLIN,
        [&]( std::uint32_t events )
        {
            seen_events = events;
            callback_count.fetch_add( kOneCallback, std::memory_order_relaxed );

            eventfd_t value = kEventfdInitialValue;
            EXPECT_EQ( ::eventfd_read( event_fd.get(), &value ), kPosixSuccess );
            callback_ran.set_value();
            reactor.stop();
        }
    );
    EXPECT_NE( token, kNoToken );

    std::thread reactor_thread(
        [&]
        {
            reactor_started.set_value( std::this_thread::get_id() );
            run_result = reactor.run();
        }
    );

    if( reactor_started_future.wait_for( kThreadReadyTimeout ) !=
        std::future_status::ready )
    {
        reactor.stop();
        reactor_thread.join();
        FAIL() << "reactor thread did not start";
    }
    EXPECT_EQ( ::eventfd_write( event_fd.get(), kEventfdSignalValue ), kPosixSuccess );

    if( callback_future.wait_for( kCallbackTimeout ) != std::future_status::ready )
    {
        reactor.stop();
        reactor_thread.join();
        FAIL() << "readable fd callback did not run";
    }

    reactor_thread.join();
    EXPECT_TRUE( run_result.has_value() );
    EXPECT_EQ( callback_count.load( std::memory_order_relaxed ), kOneCallback );
    EXPECT_NE( seen_events & EPOLLIN, kNoEvents );
}

TEST( Reactor,
      PostFromAnotherThreadRunsOnReactorThread )
{
    grab::core::Reactor           reactor;

    std::promise<std::thread::id> reactor_started;
    auto                          reactor_started_future = reactor_started.get_future();
    std::promise<std::thread::id> callback_thread;
    auto                          callback_thread_future = callback_thread.get_future();
    grab::Result<void>            run_result;

    std::thread                   reactor_thread(
        [&]
        {
            reactor_started.set_value( std::this_thread::get_id() );
            run_result = reactor.run();
        }
    );

    if( reactor_started_future.wait_for( kThreadReadyTimeout ) !=
        std::future_status::ready )
    {
        reactor.stop();
        reactor_thread.join();
        FAIL() << "reactor thread did not start";
    }
    const auto  reactor_thread_id = reactor_started_future.get();

    std::thread poster_thread(
        [&]
        {
            reactor.post(
                [&]
                {
                    callback_thread.set_value( std::this_thread::get_id() );
                    reactor.stop();
                }
            );
        }
    );
    poster_thread.join();

    if( callback_thread_future.wait_for( kCallbackTimeout ) !=
        std::future_status::ready )
    {
        reactor.stop();
        reactor_thread.join();
        FAIL() << "posted callback did not run";
    }
    const auto callback_thread_id = callback_thread_future.get();

    reactor_thread.join();
    EXPECT_TRUE( run_result.has_value() );
    EXPECT_EQ( callback_thread_id, reactor_thread_id );
}

TEST( Reactor,
      TimerFiresOnceAfterDelayOnReactorThread )
{
    grab::core::Reactor           reactor;

    std::promise<std::thread::id> reactor_started;
    auto                          reactor_started_future = reactor_started.get_future();
    std::promise<void>            timer_fired;
    auto                          timer_fired_future = timer_fired.get_future();
    grab::Result<void>            run_result;
    int                           timer_count = kNoCallbacks;
    std::thread::id               timer_thread_id;
    std::chrono::steady_clock::duration elapsed =
        std::chrono::steady_clock::duration::zero();
    const auto started_at = std::chrono::steady_clock::now();

    const auto token =
        reactor.add_timer( kTimerDelay,
                           [&]
                           {
                               ++timer_count;
                               timer_thread_id = std::this_thread::get_id();
                               elapsed = std::chrono::steady_clock::now() - started_at;
                               timer_fired.set_value();
                               reactor.stop();
                           } );
    EXPECT_NE( token, kNoToken );

    std::thread reactor_thread(
        [&]
        {
            reactor_started.set_value( std::this_thread::get_id() );
            run_result = reactor.run();
        }
    );

    if( reactor_started_future.wait_for( kThreadReadyTimeout ) !=
        std::future_status::ready )
    {
        reactor.stop();
        reactor_thread.join();
        FAIL() << "reactor thread did not start";
    }
    const auto reactor_thread_id = reactor_started_future.get();

    if( timer_fired_future.wait_for( kCallbackTimeout ) != std::future_status::ready )
    {
        reactor.stop();
        reactor_thread.join();
        FAIL() << "timer callback did not run";
    }

    reactor_thread.join();
    EXPECT_TRUE( run_result.has_value() );
    EXPECT_EQ( timer_count, kOneCallback );
    EXPECT_EQ( timer_thread_id, reactor_thread_id );
    EXPECT_GE( elapsed, kTimerDelay );
}

TEST( Reactor,
      RemoveFdStopsFurtherCallbacks )
{
    grab::core::Reactor reactor;
    UniqueFd            event_fd( ::eventfd( kEventfdInitialValue, EFD_CLOEXEC ) );
    ASSERT_NE( event_fd.get(), kInvalidFd );

    std::promise<std::thread::id> reactor_started;
    auto                          reactor_started_future = reactor_started.get_future();
    std::promise<void>            first_callback_ran;
    auto               first_callback_future = first_callback_ran.get_future();
    std::promise<void> remove_drained;
    auto               remove_drained_future = remove_drained.get_future();
    grab::Result<void> run_result;
    std::atomic<int>   callback_count{ kNoCallbacks };

    const auto         token = reactor.add_fd(
        event_fd.get(),
        EPOLLIN,
        [&]( std::uint32_t events )
        {
            EXPECT_NE( events & EPOLLIN, kNoEvents );
            callback_count.fetch_add( kOneCallback, std::memory_order_relaxed );

            eventfd_t value = kEventfdInitialValue;
            EXPECT_EQ( ::eventfd_read( event_fd.get(), &value ), kPosixSuccess );
            first_callback_ran.set_value();
        }
    );
    EXPECT_NE( token, kNoToken );

    std::thread reactor_thread(
        [&]
        {
            reactor_started.set_value( std::this_thread::get_id() );
            run_result = reactor.run();
        }
    );

    if( reactor_started_future.wait_for( kThreadReadyTimeout ) !=
        std::future_status::ready )
    {
        reactor.stop();
        reactor_thread.join();
        FAIL() << "reactor thread did not start";
    }

    EXPECT_EQ( ::eventfd_write( event_fd.get(), kEventfdSignalValue ), kPosixSuccess );
    if( first_callback_future.wait_for( kCallbackTimeout ) != std::future_status::ready )
    {
        reactor.stop();
        reactor_thread.join();
        FAIL() << "initial fd callback did not run";
    }

    reactor.remove_fd( token );
    reactor.post(
        [&]
        {
            remove_drained.set_value();
        }
    );
    if( remove_drained_future.wait_for( kCallbackTimeout ) != std::future_status::ready )
    {
        reactor.stop();
        reactor_thread.join();
        FAIL() << "remove_fd did not drain";
    }

    EXPECT_EQ( ::eventfd_write( event_fd.get(), kEventfdSignalValue ), kPosixSuccess );
    std::this_thread::sleep_for( kNoCallbackWindow );
    reactor.stop();
    reactor_thread.join();

    EXPECT_TRUE( run_result.has_value() );
    EXPECT_EQ( callback_count.load( std::memory_order_relaxed ), kOneCallback );
}

TEST( Reactor,
      StopFromAnotherThreadMakesRunReturnAndIsIdempotent )
{
    grab::core::Reactor           reactor;

    std::promise<std::thread::id> reactor_started;
    auto                          reactor_started_future = reactor_started.get_future();
    grab::Result<void>            run_result;

    std::thread                   reactor_thread(
        [&]
        {
            reactor_started.set_value( std::this_thread::get_id() );
            run_result = reactor.run();
        }
    );

    if( reactor_started_future.wait_for( kThreadReadyTimeout ) !=
        std::future_status::ready )
    {
        reactor.stop();
        reactor_thread.join();
        FAIL() << "reactor thread did not start";
    }

    std::thread stopper_thread(
        [&]
        {
            reactor.stop();
        }
    );
    stopper_thread.join();

    reactor_thread.join();
    reactor.stop();

    EXPECT_TRUE( run_result.has_value() );
}
