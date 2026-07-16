#include "drivers/desktop/x11/x11_runtime.hpp"
#include "grab/context.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/locator.hpp"
#include "grab/query.hpp"
#include "grab/relation.hpp"
#include "grab/result.hpp"
#include "grab/session.hpp"
#include "kernel/graph/target_registry.hpp"
#include "kernel/graph/tree_store.hpp"
#include "kernel/lifecycle/session_impl.hpp"
#include "kernel/query/evaluator.hpp"
#include "kernel/query/snapshot_tree_nav.hpp"
#include "spi/runtime.hpp"
#include "spi/tree_source.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <memory>
#include <utility>

namespace grab::kernel::lifecycle
{
    namespace
    {

        [[nodiscard]]
        EventKind
        event_kind( kernel::TreeEventKind kind ) noexcept
        {
            switch( kind )
            {
                case kernel::TreeEventKind::NodeAdded :
                    return EventKind::NodeAdded;
                case kernel::TreeEventKind::NodeRemoved :
                    return EventKind::NodeRemoved;
                case kernel::TreeEventKind::NodeChanged :
                    return EventKind::NodeChanged;
                case kernel::TreeEventKind::RelationAdded :
                    return EventKind::RelationAdded;
                case kernel::TreeEventKind::RelationRemoved :
                    return EventKind::RelationRemoved;
            }
            return EventKind::Unspecified;
        }

    }    // namespace

    SessionCore::SessionCore() :
        store_(
            [this]( const kernel::TreeEvent& event )
            {
                publish_tree_event( event );
            }
        )
    {
    }

    SessionCore::~SessionCore()
    {
        if( primary_runtime_ == nullptr )
        {
            return;
        }

        try
        {
            static_cast<void>(
                primary_runtime_->stop()
            );    // NOLINT(bugprone-unused-return-value)
        }
        catch( ... )
        {
            // Runtime shutdown must not escape this destructor.
            return;
        }
    }

    Result<std::unique_ptr<SessionCore>>
    SessionCore::open( const SessionOptions& options )
    {
        // X11 currently connects through DISPLAY (XcbConnection::open( "" )).
        // options.display is an availability signal until the runtime accepts an
        // explicit display string (Wave-1 Task 6/8).
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        if( !options.display.has_value() && std::getenv( "DISPLAY" ) == nullptr )
        {
            return fail( ErrorCode::DisplayUnavailable,
                         "no display available for session composition" );
        }

        auto runtime = std::make_unique<grab::drivers::desktop::x11::X11Runtime>();
        const OperationContext context{};
        auto                   started = runtime->start( context );
        if( !started.has_value() )
        {
            return std::unexpected( std::move( started.error() ) );
        }

        auto core = std::unique_ptr<SessionCore>( new SessionCore() );

        // The X11 runtime owns the TargetRegistry its tree source writes window
        // aliases into; expose that registry instead of owning a second,
        // divergent instance. Test cores own their own registry.
        core->registry_        = runtime->target_registry();
        core->primary_runtime_ = std::move( runtime );

        auto attached          = core->attach( *core->primary_runtime_, context );
        if( !attached.has_value() )
        {
            return std::unexpected( std::move( attached.error() ) );
        }
        return core;
    }

    std::unique_ptr<SessionCore>
    SessionCore::open_for_test()
    {
        return std::unique_ptr<SessionCore>( new SessionCore() );
    }

    Result<Match>
    SessionCore::resolve( const Locator& locator,
                          Cardinality    cardinality )
    {
        auto snapshot = store_.snapshot();
        if( !snapshot.has_value() )
        {
            return fail( ErrorCode::CapabilityUnavailable,
                         "session has no tree snapshot" );
        }
        const query::SnapshotTreeNav navigation{ *snapshot };
        return query::resolve( locator,
                               cardinality,
                               query::QueryScope{ .navigation = navigation } );
    }

    Result<Subscription>
    SessionCore::watch( SubscriptionScope scope,
                        QueueOptions      options )
    {
        return bus_.subscribe( std::move( scope ), options );
    }

    Result<void>
    SessionCore::attach( spi::Runtime&           runtime,
                         const OperationContext& context )
    {
        auto* const source = runtime.tree_source();
        if( source == nullptr )
        {
            return fail( ErrorCode::CapabilityUnavailable,
                         "runtime has no tree source" );
        }

        auto drained = drain_source( *source, context );
        if( !drained.has_value() )
        {
            return std::unexpected( std::move( drained.error() ) );
        }

        if( *drained == 0U )
        {
            static constexpr std::uint32_t primaryTree = 1U;

            // The X11 tree source has no update stream yet (Task 8), so prime it
            // with an explicit snapshot. Fake/test sources queue their initial
            // snapshot as an update.
            auto snapshot_result = source->snapshot( primaryTree, context );
            if( !snapshot_result.has_value() )
            {
                return std::unexpected( std::move( snapshot_result.error() ) );
            }

            auto applied = store_.apply( spi::UiUpdate{
                .source_sequence = 0U,
                .payload         = std::move( *snapshot_result ),
            } );
            if( !applied.has_value() )
            {
                return std::unexpected( std::move( applied.error() ) );
            }
        }

        attached_.push_back( source );
        return {};
    }

    Result<std::size_t>
    SessionCore::drain_source( spi::TreeSource&        source,
                               const OperationContext& context )
    {
        std::size_t applied_updates{};
        for( ;; )
        {
            auto update = source.next_update( context );
            if( !update.has_value() )
            {
                return std::unexpected( std::move( update.error() ) );
            }
            if( !update->has_value() )
            {
                return applied_updates;
            }

            auto applied = store_.apply( **update );
            if( !applied.has_value() )
            {
                return std::unexpected( std::move( applied.error() ) );
            }
            ++applied_updates;
        }
    }

    Result<void>
    SessionCore::pump_once( const OperationContext& context )
    {
        for( auto* const source : attached_ )
        {
            auto drained = drain_source( *source, context );
            if( !drained.has_value() )
            {
                return std::unexpected( std::move( drained.error() ) );
            }
        }
        return {};
    }

    void
    SessionCore::publish_tree_event( const kernel::TreeEvent& event )
    {
        // TreeStore calls this outside its lock from exactly one drainer at a
        // time, so the translation state needs no extra locking.
        const auto batch_revision = store_.revision();
        if( batch_revision != sink_batch_revision_ )
        {
            sink_previous_revision_ = sink_batch_revision_;
            sink_batch_revision_    = batch_revision;
            pending_active_.clear();
        }

        const auto make_event =
            [&event, this]( EventKind kind, std::uint64_t previous_active )
        {
            return Event{
                .kind     = kind,
                .category = EventCategory::Window,
                .payload =
                    GraphChange{
                                .node            = event.node.value,
                                .related         = event.related.value,
                                .relation        = event.relation.value,
                                .previous_active = previous_active,
                                },
                .subject =
                    EventSubject{
                                .runtime  = event.runtime,
                                .tree     = event.tree,
                                .epoch    = event.epoch,
                                .node     = event.node.value,
                                .revision = event.revision,
                                },
                .before_revision = sink_previous_revision_,
                .after_revision  = event.revision,
            };
        };

        bus_.publish( make_event( event_kind( event.kind ), 0U ) );

        if( event.kind == kernel::TreeEventKind::NodeRemoved )
        {
            std::erase_if( current_active_,
                           [&event]( const auto& entry )
                           {
                               return entry.first == event.node.value;
                           } );
        }

        if( event.relation != relation::active_child )
        {
            return;
        }

        // Graph-diff iteration does not order changes to distinct edges, so an
        // active-child Add may precede its paired Remove within one batch.
        const auto find_parent = [&event]( auto& entries )
        {
            return std::ranges::find_if( entries,
                                         [&event]( const auto& entry )
                                         {
                                             return entry.first == event.node.value;
                                         } );
        };

        const auto pending = find_parent( pending_active_ );
        if( event.kind == kernel::TreeEventKind::RelationRemoved )
        {
            if( pending == pending_active_.end() )
            {
                pending_active_.emplace_back( event.node.value, event.related.value );
            }
            else
            {
                pending->second = event.related.value;
            }

            const auto current = find_parent( current_active_ );
            if( current !=
                current_active_.end() &&
                current->second == event.related.value )
            {
                current_active_.erase( current );
            }
            return;
        }

        if( event.kind != kernel::TreeEventKind::RelationAdded )
        {
            return;
        }

        std::uint64_t previous_active{};
        if( pending != pending_active_.end() )
        {
            previous_active = pending->second;
            pending_active_.erase( pending );
        }
        else
        {
            const auto current = find_parent( current_active_ );
            if( current != current_active_.end() )
            {
                previous_active = current->second;
            }
        }
        bus_.publish( make_event( EventKind::ActiveChildChanged, previous_active ) );

        const auto current = find_parent( current_active_ );
        if( current == current_active_.end() )
        {
            current_active_.emplace_back( event.node.value, event.related.value );
        }
        else
        {
            current->second = event.related.value;
        }
    }

    EventBus&
    SessionCore::bus() noexcept
    {
        return bus_;
    }

    TreeStore&
    SessionCore::store() noexcept
    {
        return store_;
    }

    TargetRegistry&
    SessionCore::registry() noexcept
    {
        return *registry_;
    }

    spi::Runtime&
    SessionCore::primary_runtime() noexcept
    {
        return *primary_runtime_;
    }

    Result<Match>
    resolve_verb( SessionCore*   core,
                  const Locator& locator,
                  Cardinality    cardinality )
    {
        if( core == nullptr )
        {
            return fail( ErrorCode::CapabilityUnavailable,
                         "session has no composed display stack" );
        }
        return core->resolve( locator, cardinality );
    }

    Result<Subscription>
    watch_verb( SessionCore*      core,
                SubscriptionScope scope,
                QueueOptions      options )
    {
        if( core == nullptr )
        {
            return fail( ErrorCode::CapabilityUnavailable,
                         "session has no composed display stack" );
        }
        return core->watch( std::move( scope ), options );
    }

}    // namespace grab::kernel::lifecycle
