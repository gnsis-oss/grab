#include "drivers/desktop/x11/x11_capture_route.hpp"
#include "drivers/desktop/x11/x11_runtime.hpp"
#include "drivers/semantic/atspi/atspi_runtime.hpp"
#include "grab/capture.hpp"
#include "grab/context.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/event_descriptor.hpp"
#include "grab/interaction.hpp"
#include "grab/locator.hpp"
#include "grab/query.hpp"
#include "grab/relation.hpp"
#include "grab/result.hpp"
#include "grab/session.hpp"
#include "grab/trace.hpp"
#include "kernel/action/transaction.hpp"
#include "kernel/graph/target_registry.hpp"
#include "kernel/graph/tree_store.hpp"
#include "kernel/lifecycle/session_impl.hpp"
#include "kernel/query/evaluator.hpp"
#include "kernel/query/snapshot_tree_nav.hpp"
#include "spi/event_source.hpp"
#include "spi/runtime.hpp"
#include "spi/tree_source.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace grab::kernel::lifecycle
{
    namespace
    {

        // Mirrors X11TreeSource::firstTree
        // (src/drivers/desktop/x11/x11_tree_source.hpp): the single tree id every
        // runtime tree source publishes today. Keep in sync.
        constexpr std::uint32_t primaryTree = 1U;

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
        try
        {
            // User-held subscriptions may outlive this core; their teardown must
            // not re-enter a destroyed runtime.
            bus_.set_demand_callback( {} );
            if( owned_runtime_ == nullptr )
            {
                return;
            }

            static_cast<void>(
                owned_runtime_->stop()
            );    // NOLINT(bugprone-unused-return-value)
        }
        catch( ... )
        {
            // Runtime shutdown must not escape this destructor.
            return;
        }
    }

    Result<std::unique_ptr<SessionCore>>
    SessionCore::open( const SessionOptions& options,
                       grab::core::Reactor*  reactor )
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
        auto* const            x11 = runtime.get();
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
        core->owned_runtime_   = std::move( runtime );
        core->x11_runtime_     = x11;
        core->primary_runtime_ = core->owned_runtime_.get();

        auto attached          = core->attach( *core->owned_runtime_, context );
        if( !attached.has_value() )
        {
            return std::unexpected( std::move( attached.error() ) );
        }

        const auto* const capture_error = x11->capture_route_error();
        if( capture_error != nullptr )
        {
            core->runtime_diagnostics_.push_back( DiagnosticEntry{
                .at      = std::chrono::steady_clock::now(),
                .message = std::string{ "x11 capture route unavailable: " } +
                           capture_error->message,
            } );
        }
        core->compose_atspi_best_effort( reactor, context );

        // bus_ is declared before owned_runtime_, so it outlives the runtime and
        // remains valid for every event-source callback.
        x11->set_event_sink(
            [&bus = core->bus_]( Event&& event )
            {
                bus.publish( std::move( event ) );
            }
        );

        // Demand callbacks return void and run on subscriber threads, where the
        // diagnostics vector is not synchronized, so toggling failures are
        // swallowed as best-effort. Only input kinds map to XI2 masks; the X11
        // event source no-ops all other event names.
        core->bus_.set_demand_callback(
            [x11]( EventKind kind, bool enabled )
            {
                auto* const source = x11->event_source();
                if( source == nullptr )
                {
                    return;
                }

                const spi::EventSpec spec{
                    .name = std::string{ wire_name( kind ) },
                };
                if( enabled )
                {
                    static_cast<void>(
                        source->enable( spec )
                    );    // NOLINT(bugprone-unused-return-value)
                }
                else
                {
                    static_cast<void>(
                        source->disable( spec )
                    );    // NOLINT(bugprone-unused-return-value)
                }
            }
        );
        return core;
    }

    void
    SessionCore::compose_atspi_best_effort( grab::core::Reactor*    reactor,
                                            const OperationContext& context )
    {
        if( reactor == nullptr )
        {
            runtime_diagnostics_.push_back( DiagnosticEntry{
                .at      = std::chrono::steady_clock::now(),
                .message = "atspi runtime skipped: session has no reactor",
            } );
            return;
        }

        grab::drivers::semantic::atspi::AtspiRuntime runtime{
            *reactor,
            bus_,
            *registry_,
        };
        auto started = runtime.start( context );
        if( !started.has_value() )
        {
            runtime_diagnostics_.push_back( DiagnosticEntry{
                .at      = std::chrono::steady_clock::now(),
                .message = std::string{ "atspi runtime unavailable: " } +
                           started.error().message,
            } );
            return;
        }

        // TreeStore is single-scope; attaching AT-SPI would retire X11
        // (src/kernel/graph/tree_store.cpp:1283, 1358-1370).
        runtime_diagnostics_.push_back( DiagnosticEntry{
            .at      = std::chrono::steady_clock::now(),
            .message = "atspi attach deferred: store is single-scope",
        } );
        static_cast<void>( runtime.stop() );    // NOLINT(bugprone-unused-return-value)
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

    Result<Receipt>
    SessionCore::perform( const Action&        action,
                          const ActionOptions& options )
    {
        if( primary_runtime_ == nullptr )
        {
            return fail( ErrorCode::CapabilityUnavailable,
                         "session has no primary runtime" );
        }
        kernel::action::MappingRefreshHook mapping_refresh{};
        if( x11_runtime_ != nullptr )
        {
            if( auto* const route = x11_runtime_->capture_route(); route != nullptr )
            {
                // Export the capture authority's SpaceGraph transforms into the
                // receipt; the route outlives the transaction (runtime-scoped).
                mapping_refresh = [route]( const Match&, const OperationContext& )
                {
                    return route->refresh_transforms();
                };
            }
        }
        const kernel::action::Transaction transaction{
            *primary_runtime_,
            primaryTree,
            std::move( mapping_refresh )
        };
        auto outcome = transaction.perform( action, options );
        if( outcome.error.has_value() )
        {
            return std::unexpected( std::move( *outcome.error ) );
        }
        return std::move( outcome.receipt );
    }

    Result<Frame>
    SessionCore::capture( const CaptureTarget& target,
                          CaptureOptions       options )
    {
        // Admission-time deadline check; the synchronous XShm capture path
        // does not observe mid-flight deadlines yet.
        const OperationContext context{
            .deadline = Deadline::after( options.deadline ),
        };
        if( auto admitted = context.check(); !admitted.has_value() )
        {
            return std::unexpected( std::move( admitted.error() ) );
        }
        if( x11_runtime_ == nullptr )
        {
            return fail( ErrorCode::CapabilityUnavailable,
                         "session has no display runtime for capture" );
        }
        auto* const route = x11_runtime_->capture_route();
        if( route == nullptr )
        {
            // Surface the recorded open failure detail when there is one.
            const auto* const open_error = x11_runtime_->capture_route_error();
            std::string       message    = "session has no capture route";
            if( open_error != nullptr )
            {
                message += ": ";
                message += open_error->message;
            }
            return fail( ErrorCode::CapabilityUnavailable, std::move( message ) );
        }
        if( const auto* const output = std::get_if<std::string>( &target ) )
        {
            return route->capture_output( *output );
        }
        const auto& match  = std::get<Match>( target );
        auto        native = x11_runtime_->resolve_native_window( match.ref );
        if( !native.has_value() )
        {
            return std::unexpected( std::move( native.error() ) );
        }
        return route->capture_window( *native );
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
        // Test cores gain their primary runtime from the first attach; open()
        // cores already set the owned runtime.
        if( primary_runtime_ == nullptr )
        {
            primary_runtime_ = &runtime;
        }
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

    const std::vector<DiagnosticEntry>&
    SessionCore::runtime_diagnostics() const noexcept
    {
        return runtime_diagnostics_;
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

    Result<Receipt>
    perform_verb( SessionCore*         core,
                  const Action&        action,
                  const ActionOptions& options )
    {
        if( core == nullptr )
        {
            return fail( ErrorCode::CapabilityUnavailable,
                         "session has no composed display stack" );
        }
        return core->perform( action, options );
    }

    Result<Frame>
    capture_verb( SessionCore*         core,
                  const CaptureTarget& target,
                  CaptureOptions       options )
    {
        if( core == nullptr )
        {
            return fail( ErrorCode::CapabilityUnavailable,
                         "session has no composed display stack" );
        }
        return core->capture( target, options );
    }

}    // namespace grab::kernel::lifecycle
