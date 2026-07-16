#include "grab/context.hpp"
#include "grab/result.hpp"
#include "kernel/action/polling_event_source.hpp"
#include "spi/event_source.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stop_token>

namespace grab::kernel::action
{

    grab::Result<void>
    PollingEventSource::enable( const grab::spi::EventSpec& /*spec*/ )
    {
        return {};
    }

    grab::Result<void>
    PollingEventSource::disable( const grab::spi::EventSpec& /*spec*/ )
    {
        return {};
    }

    grab::Result<void>
    PollingEventSource::wait_for_event( const grab::spi::EventSpec& /*spec*/,
                                        const grab::OperationContext& context,
                                        std::chrono::nanoseconds      maximum_wait )
    {
        if( context.stop.stop_requested() )
        {
            return {};
        }

        auto       budget    = maximum_wait;
        const auto remaining = context.deadline.remaining();
        if( remaining < budget )
        {
            budget = remaining;
        }
        if( budget <= std::chrono::nanoseconds::zero() )
        {
            return {};
        }

        std::mutex                   mutex;
        std::condition_variable_any  condition;
        std::unique_lock<std::mutex> lock( mutex );
        const std::stop_callback     wake( context.stop,
                                           [&condition]
                                           {
                                           condition.notify_all();
                                           } );
        static_cast<void>( condition.wait_for( lock, budget ) );
        return {};
    }

}    // namespace grab::kernel::action
