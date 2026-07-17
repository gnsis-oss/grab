#include "drivers/desktop/x11/x11_capture_route.hpp"
#include "drivers/desktop/x11/x11_runtime.hpp"
#include "drivers/semantic/atspi/atspi_runtime.hpp"
#include "grab/capture.hpp"
#include "grab/context.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/event_descriptor.hpp"
#include "grab/ids.hpp"
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
#include <optional>
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

    SessionCore::SessionCore()
    {
        make_binding();
    }

    SessionCore::RuntimeBinding&
    SessionCore::make_binding()
    {
        auto        binding         = std::make_unique<RuntimeBinding>();
        auto* const binding_pointer = binding.get();
        binding->store              = std::make_unique<TreeStore>(
            [this, binding_pointer]( const kernel::TreeEvent& event )
            {
                publish_tree_event( *binding_pointer, event );
            }
        );
        bindings_.push_back( std::move( binding ) );
        return *binding_pointer;
    }

    RuntimeId
    SessionCore::allocate_runtime_id() noexcept
    {
        return RuntimeId{ next_runtime_id_++ };
    }

    RuntimeId
    SessionCore::runtime_id_at( std::size_t index ) const noexcept
    {
        if( index >= bindings_.size() )
        {
            return RuntimeId{};
        }
        return bindings_[index]->assigned_runtime;
    }

    SessionCore::~SessionCore()
    {
        try
        {
            // User-held subscriptions may outlive this core; their teardown must
            // not re-enter a destroyed runtime.
            bus_.set_demand_callback( {} );
            if( atspi_runtime_ != nullptr )
            {
                static_cast<void>(
                    atspi_runtime_->stop()
                );    // NOLINT(bugprone-unused-return-value)
            }

            if( owned_runtime_ != nullptr )
            {
                static_cast<void>(
                    owned_runtime_->stop()
                );    // NOLINT(bugprone-unused-return-value)
            }
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

        // bus_ is declared before both runtime owners, so it outlives them and
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

        const auto atspi_runtime_id = allocate_runtime_id();
        auto runtime = std::make_unique<grab::drivers::semantic::atspi::AtspiRuntime>(
            *reactor,
            bus_,
            *registry_,
            grab::drivers::semantic::atspi::AtspiTreeSource::AccessibleEnumerator{},
            std::nullopt,
            atspi_runtime_id
        );
        auto started = runtime->start( context );
        if( !started.has_value() )
        {
            runtime_diagnostics_.push_back( DiagnosticEntry{
                .at      = std::chrono::steady_clock::now(),
                .message = std::string{ "atspi runtime unavailable: " } +
                           started.error().message,
            } );
            return;
        }

        auto attached = attach( *runtime, context, atspi_runtime_id );
        if( !attached.has_value() )
        {
            runtime_diagnostics_.push_back( DiagnosticEntry{
                .at = std::chrono::steady_clock::now(),
                .message =
                    std::string{ "atspi attach failed: " } + attached.error().message,
            } );
            static_cast<void>(
                runtime->stop()
            );    // NOLINT(bugprone-unused-return-value)
            return;
        }
        atspi_runtime_ = std::move( runtime );
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
        auto snapshot = bindings_.front()->store->snapshot();
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

    Result<NodeInfo>
    SessionCore::describe( const Match& match )
    {
        auto snapshot = bindings_.front()->store->snapshot();
        if( !snapshot.has_value() )
        {
            return fail( ErrorCode::CapabilityUnavailable,
                         "session has no tree snapshot" );
        }

        const auto* const record = snapshot->node( NodeId{ match.ref.node } );
        if( record == nullptr )
        {
            return fail( ErrorCode::NoMatch,
                         "resolved node is not present in the current snapshot" );
        }
        if( record->generation != match.ref.generation )
        {
            return fail( ErrorCode::StaleNode, "resolved node generation is stale" );
        }

        NodeInfo info{};
        info.role       = record->role;
        info.states     = record->states;
        info.provenance = record->provenance();

        const auto read = record->property( grab::property::bounds );
        if( read.state == PropertyRead::State::Present )
        {
            if( const auto* const rect = std::get_if<SpaceRect>( &read.value ) )
            {
                info.bounds = *rect;
            }
        }
        return info;
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
    SessionCore::attach( spi::Runtime&            runtime,
                         const OperationContext&  context,
                         std::optional<RuntimeId> assigned_runtime )
    {
        // Re-attaching an already-bound runtime (e.g. after a restart) is
        // idempotent and preserves the session id already assigned to it.
        for( const auto& existing : bindings_ )
        {
            if( existing->runtime == &runtime )
            {
                return {};
            }
        }

        auto* const source = runtime.tree_source();
        if( source == nullptr )
        {
            return fail( ErrorCode::CapabilityUnavailable,
                         "runtime has no tree source" );
        }

        auto* binding = bindings_.front().get();
        bool  binding_is_new{};
        if( binding->source != nullptr )
        {
            binding        = &make_binding();
            binding_is_new = true;
        }

        binding->assigned_runtime = assigned_runtime.value_or( allocate_runtime_id() );

        auto drained              = drain_source( *source, *binding->store, context );
        if( !drained.has_value() )
        {
            if( binding_is_new )
            {
                bindings_.pop_back();
            }
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
                if( binding_is_new )
                {
                    bindings_.pop_back();
                }
                return std::unexpected( std::move( snapshot_result.error() ) );
            }

            auto applied = binding->store->apply( spi::UiUpdate{
                .source_sequence = 0U,
                .payload         = std::move( *snapshot_result ),
            } );
            if( !applied.has_value() )
            {
                if( binding_is_new )
                {
                    bindings_.pop_back();
                }
                return std::unexpected( std::move( applied.error() ) );
            }
        }

        binding->runtime = &runtime;
        binding->source  = source;
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
                               TreeStore&              store,
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

            auto applied = store.apply( **update );
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
        for( const auto& binding : bindings_ )
        {
            if( binding->source == nullptr )
            {
                continue;
            }
            auto drained = drain_source( *binding->source, *binding->store, context );
            if( !drained.has_value() )
            {
                return std::unexpected( std::move( drained.error() ) );
            }
        }
        return {};
    }

    void
    SessionCore::publish_tree_event( RuntimeBinding&          binding,
                                     const kernel::TreeEvent& event )
    {
        // Each TreeStore invokes its sink outside its lock from exactly one
        // drainer at a time. Bindings do not share translation state; bus_ is
        // thread-safe.
        const auto batch_revision = binding.store->revision();
        if( batch_revision != binding.sink_batch_revision )
        {
            binding.sink_previous_revision = binding.sink_batch_revision;
            binding.sink_batch_revision    = batch_revision;
            binding.pending_active.clear();
        }

        const auto make_event =
            [&binding, &event]( EventKind kind, std::uint64_t previous_active )
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
                                .runtime  = binding.assigned_runtime,
                                .tree     = event.tree,
                                .epoch    = event.epoch,
                                .node     = event.node.value,
                                .revision = event.revision,
                                },
                .before_revision = binding.sink_previous_revision,
                .after_revision  = event.revision,
            };
        };

        bus_.publish( make_event( event_kind( event.kind ), 0U ) );

        if( event.kind == kernel::TreeEventKind::NodeRemoved )
        {
            std::erase_if( binding.current_active,
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

        const auto pending = find_parent( binding.pending_active );
        if( event.kind == kernel::TreeEventKind::RelationRemoved )
        {
            if( pending == binding.pending_active.end() )
            {
                binding.pending_active.emplace_back( event.node.value,
                                                     event.related.value );
            }
            else
            {
                pending->second = event.related.value;
            }

            const auto current = find_parent( binding.current_active );
            if( current !=
                binding.current_active.end() &&
                current->second == event.related.value )
            {
                binding.current_active.erase( current );
            }
            return;
        }

        if( event.kind != kernel::TreeEventKind::RelationAdded )
        {
            return;
        }

        std::uint64_t previous_active{};
        if( pending != binding.pending_active.end() )
        {
            previous_active = pending->second;
            binding.pending_active.erase( pending );
        }
        else
        {
            const auto current = find_parent( binding.current_active );
            if( current != binding.current_active.end() )
            {
                previous_active = current->second;
            }
        }
        bus_.publish( make_event( EventKind::ActiveChildChanged, previous_active ) );

        const auto current = find_parent( binding.current_active );
        if( current == binding.current_active.end() )
        {
            binding.current_active.emplace_back( event.node.value, event.related.value );
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
        return *bindings_.front()->store;
    }

    std::size_t
    SessionCore::store_count() const noexcept
    {
        return bindings_.size();
    }

    TreeStore*
    SessionCore::store_at( std::size_t index ) noexcept
    {
        if( index >= bindings_.size() )
        {
            return nullptr;
        }
        return bindings_[index]->store.get();
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

    Result<NodeInfo>
    describe_verb( SessionCore* core,
                   const Match& match )
    {
        if( core == nullptr )
        {
            return fail( ErrorCode::CapabilityUnavailable,
                         "session has no composed display stack" );
        }
        return core->describe( match );
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
