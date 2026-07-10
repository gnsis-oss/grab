#include "grab/result.hpp"
#include "grab/session.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <chrono>
#include <future>
#include <thread>
#include <utility>
// clang-format on

namespace
{

    constexpr auto callbackTimeout   = std::chrono::seconds{ 2 };
    constexpr auto sessionClosedCode = grab::ErrorCode::SessionClosed;

}    // namespace

TEST( Session,
      OpenStartsReactorThreadAndPostRuns )
{
    std::promise<std::thread::id> callback_thread;
    auto                          callback_thread_future = callback_thread.get_future();
    const auto                    caller_thread_id       = std::this_thread::get_id();

    auto                          session_result         = grab::Session::open();
    ASSERT_TRUE( session_result.has_value() );
    auto session = std::move( *session_result );
    ASSERT_TRUE( session->is_open() );

    const auto post_result = session->post(
        [&callback_thread]
        {
            callback_thread.set_value( std::this_thread::get_id() );
        }
    );
    ASSERT_TRUE( post_result.has_value() );

    if( callback_thread_future.wait_for( callbackTimeout ) != std::future_status::ready )
    {
        session->close();
        FAIL() << "posted callback did not run";
    }

    EXPECT_NE( callback_thread_future.get(), caller_thread_id );
    session->close();
    EXPECT_FALSE( session->is_open() );
}

TEST( Session,
      CloseStopsAndIsIdempotent )
{
    auto session_result = grab::Session::open();
    ASSERT_TRUE( session_result.has_value() );
    auto session = std::move( *session_result );

    session->close();
    EXPECT_FALSE( session->is_open() );

    session->close();
    EXPECT_FALSE( session->is_open() );
}

TEST( Session,
      PostAfterCloseReturnsSessionClosed )
{
    auto session_result = grab::Session::open();
    ASSERT_TRUE( session_result.has_value() );
    auto session = std::move( *session_result );

    session->close();

    const auto post_result = session->post(
        []
        {
        }
    );
    ASSERT_FALSE( post_result.has_value() );
    EXPECT_EQ( post_result.error().code, sessionClosedCode );
}

TEST( Session,
      DestructorClosesCleanly )
{
    std::promise<void> task_ran;
    auto               task_ran_future = task_ran.get_future();

    {
        auto session_result = grab::Session::open();
        ASSERT_TRUE( session_result.has_value() );
        auto session = std::move( *session_result );
        ASSERT_TRUE( session->is_open() );

        const auto post_result = session->post(
            [&task_ran]
            {
                task_ran.set_value();
            }
        );
        ASSERT_TRUE( post_result.has_value() );

        if( task_ran_future.wait_for( callbackTimeout ) != std::future_status::ready )
        {
            session->close();
            FAIL() << "posted callback did not run before destruction";
        }
    }
}
