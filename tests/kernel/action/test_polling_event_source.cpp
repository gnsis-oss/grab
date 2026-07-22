#include "grab/context.hpp"
#include "grab/result.hpp"
#include "kernel/action/polling_event_source.hpp"
#include "spi/event_source.hpp"

#include <chrono>
#include <gtest/gtest.h>
#include <stop_token>

TEST( PollingEventSource,
      EnableAndDisableSucceed )
{
    grab::kernel::action::PollingEventSource source;
    const grab::spi::EventSpec               spec{ .name = "tick" };
    EXPECT_TRUE( source.enable( spec ).has_value() );
    EXPECT_TRUE( source.disable( spec ).has_value() );
}

TEST( PollingEventSource,
      WaitBlocksApproximatelyForTheBudget )
{
    grab::kernel::action::PollingEventSource source;
    const grab::spi::EventSpec               spec{ .name = "tick" };
    grab::OperationContext                   context;
    const auto                               start = std::chrono::steady_clock::now();
    const auto                               result =
        source.wait_for_event( spec, context, std::chrono::milliseconds{ 30 } );
    const auto elapsed = std::chrono::steady_clock::now() - start;
    ASSERT_TRUE( result.has_value() );
    EXPECT_GE( elapsed, std::chrono::milliseconds{ 12 } );
}

TEST( PollingEventSource,
      WaitReturnsPromptlyWhenStopRequested )
{
    grab::kernel::action::PollingEventSource source;
    const grab::spi::EventSpec               spec{ .name = "tick" };
    std::stop_source                         stop;
    stop.request_stop();
    grab::OperationContext context;
    context.stop     = stop.get_token();
    const auto start = std::chrono::steady_clock::now();
    const auto result =
        source.wait_for_event( spec, context, std::chrono::seconds{ 5 } );
    const auto elapsed = std::chrono::steady_clock::now() - start;
    static_cast<void>( result );
    EXPECT_LT( elapsed, std::chrono::seconds{ 1 } );
}
