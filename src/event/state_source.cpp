#include "core/reactor.hpp"
#include "event/source.hpp"
#include "event/state.hpp"
#include "event/state_source.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/result.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace grab::event
{
    namespace
    {

        constexpr std::string_view stateSourceName  = "state";
        constexpr auto             stateSourceKinds = std::to_array<grab::EventKind>( {
            grab::EventKind::StateSnapshot,
        } );

        [[nodiscard]]
        double
        current_timestamp()
        {
            const auto now = std::chrono::system_clock::now().time_since_epoch();
            return std::chrono::duration<double>{ now }.count();
        }

        [[nodiscard]]
        grab::Result<void>
        state_source_failure( std::string message )
        {
            return grab::fail( grab::ErrorCode::InternalFault, std::move( message ) );
        }

    }    // namespace

    struct StateSource::State
    {
            std::mutex                        mutex;
            StateManager                      manager;
            std::optional<grab::Subscription> subscription;
            grab::core::Reactor*              reactor = nullptr;
            grab::EventBus*                   bus     = nullptr;
            std::chrono::nanoseconds          interval{};
            bool                              active = false;
    };

    void
    StateSource::drain( const std::shared_ptr<State>& state )
    {
        const std::scoped_lock lock( state->mutex );
        if( !state->active || !state->subscription.has_value() )
        {
            return;
        }

        while( std::optional<grab::Event> event = state->subscription->try_pop() )
        {
            state->manager.observe( *event );
        }
    }

    void
    StateSource::publish_snapshot_once( const std::shared_ptr<State>& state )
    {
        grab::EventBus* bus = nullptr;
        {
            const std::scoped_lock lock( state->mutex );
            if( !state->active || state->bus == nullptr )
            {
                return;
            }
            bus = state->bus;
        }

        state->manager.publish_snapshot( *bus, current_timestamp() );
    }

    void
    StateSource::publish_periodic_snapshot( const std::shared_ptr<State>& state )
    {
        grab::EventBus* bus = nullptr;
        {
            const std::scoped_lock lock( state->mutex );
            if( !state->active || state->bus == nullptr )
            {
                return;
            }
            bus = state->bus;
        }

        state->manager.publish_snapshot( *bus, current_timestamp() );

        grab::core::Reactor*     reactor = nullptr;
        std::chrono::nanoseconds interval{};
        {
            const std::scoped_lock lock( state->mutex );
            if( !state->active || state->reactor == nullptr )
            {
                return;
            }
            reactor  = state->reactor;
            interval = state->interval;
        }

        static_cast<void>(
            reactor->add_timer( interval,
                                [state]
                                {
                                    StateSource::publish_periodic_snapshot( state );
                                } )
        );
    }

    void
    StateSource::post_drain( const std::weak_ptr<State>& weak_state )
    {
        auto state = weak_state.lock();
        if( state == nullptr )
        {
            return;
        }

        grab::core::Reactor* reactor = nullptr;
        {
            const std::scoped_lock lock( state->mutex );
            if( !state->active || state->reactor == nullptr )
            {
                return;
            }
            reactor = state->reactor;
        }

        reactor->post(
            [state]
            {
                StateSource::drain( state );
            }
        );
    }

    void
    StateSource::deactivate_state( const std::shared_ptr<State>& state ) noexcept
    {
        if( state == nullptr )
        {
            return;
        }

        try
        {
            const std::scoped_lock lock( state->mutex );
            state->active  = false;
            state->reactor = nullptr;
            state->bus     = nullptr;
            if( state->subscription.has_value() )
            {
                state->subscription->set_notify( {} );
            }
        }
        catch( ... )
        {
            return;
        }
    }

}    // namespace grab::event

namespace grab::event::detail
{

    grab::EventFilter
    state_source_filter()
    {
        grab::EventFilter filter;
        filter.kinds = {
            grab::EventKind::WindowCreated,
            grab::EventKind::WindowClosed,
            grab::EventKind::WindowFocusChanged,
        };
        return filter;
    }

}    // namespace grab::event::detail

namespace grab::event
{

    StateSource::StateSource( std::chrono::nanoseconds interval ) :
        interval_( interval )
    {
    }

    StateSource::~StateSource()
    {
        stop();
    }

    grab::Result<void>
    StateSource::start( grab::core::Reactor& reactor,
                        grab::EventBus&      bus )
    {
        if( state_ != nullptr )
        {
            stop();
        }

        auto next_state      = std::make_shared<State>();
        next_state->reactor  = &reactor;
        next_state->bus      = &bus;
        next_state->interval = interval_;
        next_state->active   = true;

        try
        {
            next_state->subscription.emplace(
                bus.subscribe( detail::state_source_filter() )
            );
            next_state->subscription->set_notify(
                [weak_state = std::weak_ptr<State>{ next_state }]
                {
                    StateSource::post_drain( weak_state );
                }
            );

            reactor.post(
                [state = next_state]
                {
                    StateSource::publish_snapshot_once( state );
                }
            );
            static_cast<void>(
                reactor.add_timer( next_state->interval,
                                   [state = next_state]
                                   {
                                       StateSource::publish_periodic_snapshot( state );
                                   } )
            );
        }
        catch( const std::exception& exception )
        {
            StateSource::deactivate_state( next_state );
            source_state_.store( SourceState::Failed, std::memory_order_relaxed );
            return state_source_failure( std::string{ "state source start failed: " } +
                                         exception.what() );
        }
        catch( ... )
        {
            StateSource::deactivate_state( next_state );
            source_state_.store( SourceState::Failed, std::memory_order_relaxed );
            return state_source_failure(
                "state source start failed: unknown exception"
            );
        }

        state_ = std::move( next_state );
        source_state_.store( SourceState::Running, std::memory_order_relaxed );
        return {};
    }

    void
    StateSource::stop() noexcept
    {
        // Clears the active flag (under State::mutex) and drops this source's
        // reference to State. It is not a synchronous barrier: a snapshot or
        // drain already running on the reactor thread may complete, and one
        // already-armed timer may fire once (and no-op) before the flag is
        // observed. Full quiescence of in-flight reactor callbacks comes from
        // stopping and joining the reactor afterwards, which the daemon does in
        // shutdown order (stop_all -> reactor.stop -> join). This matches the
        // WindowTracker stop contract; callbacks capture a shared_ptr<State>,
        // so nothing is dereferenced after free. Do not destroy the reactor or
        // bus immediately after stop() without first stopping+joining the
        // reactor.
        auto state = std::move( state_ );
        StateSource::deactivate_state( state );
        source_state_.store( SourceState::Stopped, std::memory_order_relaxed );
    }

    SourceState
    StateSource::state() const noexcept
    {
        return source_state_.load( std::memory_order_relaxed );
    }

    std::string_view
    StateSource::name() const noexcept
    {
        return stateSourceName;
    }

    std::span<const grab::EventKind>
    StateSource::kinds() const noexcept
    {
        return std::span<const grab::EventKind>{ stateSourceKinds };
    }

}    // namespace grab::event
