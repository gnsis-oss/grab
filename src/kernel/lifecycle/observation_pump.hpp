#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/context.hpp"
#include "grab/result.hpp"

#include <functional>
#include <stop_token>
#include <thread>
#include <vector>

namespace grab
{

    class EventBus;

}    // namespace grab

namespace grab::spi
{

    class EventSource;

}    // namespace grab::spi

namespace grab::kernel::lifecycle
{

    // Continuous observation pump: turns a runtime event source's pull-based
    // wait_for_event output into bus publications (through the source's sink),
    // and periodically drains tree deltas. One jthread per pumped event source
    // plus one tree-drain thread; all honor their stop_token for clean shutdown.
    class ObservationPump
    {
        public:

            using TreeDrain = std::function<Result<void>( const OperationContext& )>;

            ObservationPump( EventBus& bus,
                             TreeDrain drain_trees );
            ~ObservationPump();

            ObservationPump( const ObservationPump& ) = delete;
            ObservationPump&
            operator=( const ObservationPump& )  = delete;
            ObservationPump( ObservationPump&& ) = delete;
            ObservationPump&
            operator=( ObservationPump&& ) = delete;

            // Wire `source`'s sink to publish onto the bus and start a jthread
            // that loops wait_for_event until stop.
            void
            pump_event_source( spi::EventSource& source );

            // Start the periodic tree-delta drain thread.
            void
            start();

            // Request stop of all threads and join.
            void
            stop() noexcept;

        private:

            void
            run_event_loop( std::stop_token   token,
                            spi::EventSource& source );

            void
                                      run_tree_loop( std::stop_token token );

            EventBus*                 bus_;
            TreeDrain                 drain_trees_;
            std::vector<std::jthread> threads_;
    };

}    // namespace grab::kernel::lifecycle
