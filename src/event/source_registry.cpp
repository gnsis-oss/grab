#include "core/reactor.hpp"
#include "event/source.hpp"
#include "event/source_registry.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/result.hpp"

#include <algorithm>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace grab::event
{

    void
    SourceRegistry::add( std::unique_ptr<EventSource> source )
    {
        if( source == nullptr )
        {
            return;
        }

        const std::scoped_lock lock( mutex_ );
        sources_.push_back( std::move( source ) );
    }

    grab::Result<void>
    SourceRegistry::start_all( grab::core::Reactor& reactor,
                               grab::EventBus&      bus )
    {
        const std::scoped_lock lock( mutex_ );
        for( auto& source : sources_ )
        {
            // Degrade policy: a source that fails (or throws) to start must not
            // abort the others or fail the daemon. A throwing factory leaves the
            // source in its pre-start state; we simply move on.
            try
            {
                auto result = source->start( reactor, bus );
                if( !result.has_value() )
                {
                    static_cast<void>( result.error() );
                }
            }
            // Intentional degrade: a source that throws on start is skipped,
            // not fatal to the daemon.
            // NOLINTNEXTLINE(bugprone-empty-catch)
            catch( ... )
            {
            }
        }
        return {};
    }

    void
    SourceRegistry::stop_all() noexcept
    {
        try
        {
            const std::scoped_lock lock( mutex_ );
            for( auto index = sources_.size(); index > 0U; --index )
            {
                sources_.at( index - 1U )->stop();
            }
        }
        catch( ... )
        {
            return;
        }
    }

    bool
    SourceRegistry::is_kind_active( grab::EventKind kind ) const noexcept
    {
        try
        {
            const std::scoped_lock lock( mutex_ );
            return std::ranges::any_of(
                sources_,
                [kind]( const std::unique_ptr<EventSource>& source )
                {
                    const auto kinds = source->kinds();
                    return source->state() ==
                           SourceState::Running &&
                           std::ranges::find( kinds, kind ) != kinds.end();
                }
            );
        }
        catch( ... )
        {
            return false;
        }
    }

    std::vector<SourceRegistry::Status>
    SourceRegistry::statuses() const
    {
        const std::scoped_lock lock( mutex_ );

        std::vector<Status>    statuses;
        statuses.reserve( sources_.size() );
        for( const auto& source : sources_ )
        {
            statuses.push_back( Status{
                .name  = source->name(),
                .state = source->state(),
            } );
        }
        return statuses;
    }

}    // namespace grab::event
