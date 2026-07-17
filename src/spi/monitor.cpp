#include "kernel/support/environment.hpp"
#include "spi/monitor.hpp"

#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace grab::core
{

    EnvironmentMonitor::EnvironmentMonitor( Environment initial ) :
        environment_( std::move( initial ) )
    {
    }

    Environment
    EnvironmentMonitor::current() const
    {
        const std::scoped_lock lock( mutex_ );
        return environment_;
    }

    std::uint64_t
    EnvironmentMonitor::update( Environment next )
    {
        std::vector<Listener> to_notify;
        Environment           snapshot;
        {
            const std::scoped_lock lock( mutex_ );
            next.generation = environment_.generation + 1;
            environment_    = std::move( next );
            snapshot        = environment_;
            to_notify.reserve( listeners_.size() );
            for( const auto& [id, listener] : listeners_ )
            {
                to_notify.push_back( listener );
            }
        }
        for( const auto& listener : to_notify )
        {
            listener( snapshot );
        }
        return snapshot.generation;
    }

    std::uint64_t
    EnvironmentMonitor::subscribe( Listener listener )
    {
        const std::scoped_lock lock( mutex_ );
        const auto             id = next_listener_id_++;
        listeners_.emplace( id, std::move( listener ) );
        return id;
    }

    void
    EnvironmentMonitor::unsubscribe( std::uint64_t id )
    {
        const std::scoped_lock lock( mutex_ );
        listeners_.erase( id );
    }

}    // namespace grab::core
