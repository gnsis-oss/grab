#include "grab/event.hpp"
#include "kernel/events/event_bus.hpp"
#include "kernel/graph/state_snapshot_provider.hpp"

#include <chrono>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace grab::event
{
    namespace
    {

        [[nodiscard]]
        double
        current_timestamp()
        {
            const auto now = std::chrono::system_clock::now().time_since_epoch();
            return std::chrono::duration<double>{ now }.count();
        }

    }    // namespace

    StateSnapshotProvider::StateSnapshotProvider( grab::EventBus& bus ) :
        bus_( &bus )
    {
        grab::EventFilter filter;
        filter.kinds = {
            grab::EventKind::WindowCreated,
            grab::EventKind::WindowClosed,
            grab::EventKind::WindowFocusChanged,
        };
        subscription_.emplace( bus.subscribe( std::move( filter ) ) );

        bus.register_snapshot_provider( grab::EventKind::StateSnapshot,
                                        [this]() -> std::vector<grab::Event>
                                        {
                                            const std::scoped_lock lock( mutex_ );
                                            drain_locked();
                                            return {
                                                manager_.snapshot( current_timestamp() )
                                            };
                                        } );
        bus.register_snapshot_provider(
            grab::EventKind::WindowCreated,
            [this]() -> std::vector<grab::Event>
            {
                const std::scoped_lock lock( mutex_ );
                drain_locked();
                return manager_.open_window_events( current_timestamp() );
            }
        );
    }

    StateSnapshotProvider::~StateSnapshotProvider()
    {
        if( bus_ != nullptr )
        {
            // These are the real teardown barrier, not the mutex below.
            // Unregistering takes the bus lock, and the bus holds that same
            // lock across a whole snapshot-provider invocation, so once these
            // return no callback is in flight and none can start. That also
            // gives the happens-before edge with the last callback's writes to
            // manager_ and subscription_.
            bus_->unregister_snapshot_provider( grab::EventKind::StateSnapshot );
            bus_->unregister_snapshot_provider( grab::EventKind::WindowCreated );
        }

        // Deliberately NOT under mutex_. ~Subscription re-enters the bus to
        // unsubscribe, so holding the provider lock across it takes
        // (provider -> bus) while EventBusState::add takes (bus -> provider)
        // when it invokes a snapshot callback under the bus lock. That is an
        // ABBA inversion; ThreadSanitizer reports it as a potential deadlock.
        // The lock guarded nothing here — drain_locked() is reachable only
        // from the two callbacks unregistered above.
        subscription_.reset();
    }

    void
    StateSnapshotProvider::drain_locked()
    {
        if( !subscription_.has_value() )
        {
            return;
        }

        while( std::optional<grab::Event> event = subscription_->try_pop() )
        {
            manager_.observe( *event );
        }
    }

}    // namespace grab::event
