#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/event_bus.hpp"
#include "kernel/graph/state_manager.hpp"

#include <mutex>
#include <optional>

namespace grab
{

    class EventBus;

}    // namespace grab

namespace grab::event
{

    // Observes window events off the bus into a StateManager and answers replay
    // subscriptions with the current state: registers a CurrentSet snapshot provider for
    // StateSnapshot (the state.snapshot document) and for WindowCreated (one
    // window.created per currently-open window). Pull-based; no reactor. On each
    // provider call it first drains its own window-event subscription so the returned
    // snapshot is up to date.
    class StateSnapshotProvider
    {
        public:

            explicit StateSnapshotProvider( grab::EventBus& bus );
            ~StateSnapshotProvider();

            StateSnapshotProvider( const StateSnapshotProvider& ) = delete;
            StateSnapshotProvider&
            operator=( const StateSnapshotProvider& )        = delete;
            StateSnapshotProvider( StateSnapshotProvider&& ) = delete;
            StateSnapshotProvider&
            operator=( StateSnapshotProvider&& ) = delete;

        private:

            void
                                              drain_locked();

            grab::EventBus*                   bus_;
            StateManager                      manager_;
            std::optional<grab::Subscription> subscription_;
            std::mutex                        mutex_;
    };

}    // namespace grab::event
