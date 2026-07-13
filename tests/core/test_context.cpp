#include "grab/context.hpp"
#include "grab/result.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <chrono>
#include <stop_token>
#include <string>
#include <thread>
// clang-format on

namespace
{

    constexpr auto contextBudget       = std::chrono::milliseconds{ 50 };
    constexpr auto expirationWait      = std::chrono::milliseconds{ 60 };
    constexpr int  diagnosticNoteCount = 10;

}    // namespace

TEST( Context,
      NestedContextSharesBudgetAndPreservesFields )
{
    std::stop_source       src;
    grab::OperationContext outer{
        .deadline = grab::Deadline::after( contextBudget ),
        .stop     = src.get_token(),
    };
    const auto inner = outer.nested();    // derive, do not field-drop
    EXPECT_LE( inner.deadline.remaining(), contextBudget );    // same budget
    EXPECT_TRUE( inner.stop.stop_possible() );                 // stop token preserved
    EXPECT_EQ( inner.causal_parent, outer.operation );         // parentage recorded
    std::this_thread::sleep_for( expirationWait );             // test-only sleep
    EXPECT_TRUE( inner.deadline.expired() );
    EXPECT_EQ( inner.check().error().code, grab::ErrorCode::DeadlineExceeded );
}

TEST( Context,
      StopTokenCancels )
{
    std::stop_source       src;
    grab::OperationContext ctx{
        .deadline = grab::Deadline::unbounded(),
        .stop     = src.get_token(),
    };
    EXPECT_TRUE( ctx.check().has_value() );
    src.request_stop();
    EXPECT_EQ( ctx.check().error().code, grab::ErrorCode::Cancelled );
}

TEST( Context,
      DiagnosticLogBoundsAndCountsDrops )
{
    grab::DiagnosticLog log{ 4 };
    for( int i = 0; i < diagnosticNoteCount; ++i )
    {
        log.note( std::to_string( i ) );
    }
    EXPECT_EQ( log.snapshot().size(), 4U );
    EXPECT_EQ( log.dropped(), 6U );
    EXPECT_EQ( log.snapshot().back().message, "9" );
}
