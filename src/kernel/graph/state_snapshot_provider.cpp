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
            bus_->unregister_snapshot_provider( grab::EventKind::StateSnapshot );
            bus_->unregister_snapshot_provider( grab::EventKind::WindowCreated );
        }

        const std::scoped_lock lock( mutex_ );
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
