#include "grab/context.hpp"
#include "grab/event.hpp"
#include "kernel/events/event_bus.hpp"
#include "kernel/lifecycle/observation_pump.hpp"
#include "spi/event_source.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stop_token>
#include <utility>

namespace grab::kernel::lifecycle
{
    namespace
    {

        constexpr auto eventWaitSlice    = std::chrono::milliseconds{ 50 };
        constexpr auto treeDrainInterval = std::chrono::milliseconds{ 50 };

    }    // namespace

    ObservationPump::ObservationPump( EventBus& bus,
                                      TreeDrain drain_trees ) :
        bus_( &bus ),
        drain_trees_( std::move( drain_trees ) )
    {
    }

    ObservationPump::~ObservationPump()
    {
        stop();
    }

    void
    ObservationPump::pump_event_source( spi::EventSource& source )
    {
        source.set_sink(
            [bus = bus_]( Event&& event )
            {
                bus->publish( std::move( event ) );
            }
        );
        threads_.emplace_back(
            [this, &source]( std::stop_token token )
            {
                run_event_loop( std::move( token ), source );
            }
        );
    }

    void
    ObservationPump::start()
    {
        if( !drain_trees_ )
        {
            return;
        }
        threads_.emplace_back(
            [this]( std::stop_token token )
            {
                run_tree_loop( std::move( token ) );
            }
        );
    }

    void
    ObservationPump::stop() noexcept
    {
        for( auto& thread : threads_ )
        {
            thread.request_stop();
        }
        threads_.clear();    // std::jthread destructor joins each worker
    }

    void
    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    ObservationPump::run_event_loop( std::stop_token   token,
                                     spi::EventSource& source )
    {
        const spi::EventSpec spec{};
        while( !token.stop_requested() )
        {
            const OperationContext context{
                .deadline = Deadline::after( eventWaitSlice ),
            };
            // Events are delivered through the wired sink as a side effect; a
            // budget-only timeout is normal and simply re-loops.
            // NOLINTNEXTLINE(bugprone-unused-return-value)
            static_cast<void>( source.wait_for_event( spec, context, eventWaitSlice ) );
        }
    }

    void
    ObservationPump::run_tree_loop( std::stop_token token )
    {
        std::mutex                  mutex;
        std::condition_variable_any condition;
        while( !token.stop_requested() )
        {
            const OperationContext context{};
            // NOLINTNEXTLINE(bugprone-unused-return-value)
            static_cast<void>( drain_trees_( context ) );

            std::unique_lock lock{ mutex };
            static_cast<void>( condition.wait_for( lock,
                                                   token,
                                                   treeDrainInterval,
                                                   [&token]()
                                                   {
                                                       return token.stop_requested();
                                                   } ) );
        }
    }

}    // namespace grab::kernel::lifecycle
