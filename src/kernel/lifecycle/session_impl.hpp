#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/capture.hpp"
#include "grab/context.hpp"
#include "grab/ids.hpp"
#include "grab/interaction.hpp"
#include "grab/locator.hpp"
#include "grab/query.hpp"
#include "grab/result.hpp"
#include "grab/session.hpp"
#include "grab/trace.hpp"
#include "kernel/events/event_bus.hpp"
#include "kernel/graph/target_registry.hpp"
#include "kernel/graph/tree_store.hpp"
#include "spi/runtime.hpp"
#include "spi/tree_source.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace grab::core
{

    class Reactor;

}

namespace grab::drivers::desktop::x11
{

    class X11Runtime;

}

namespace grab::kernel::lifecycle
{

    class ObservationPump;

    class SessionCore
    {
        public:

            [[nodiscard]]
            static Result<std::unique_ptr<SessionCore>>
            open( const SessionOptions& options,
                  grab::core::Reactor*  reactor = nullptr );

            [[nodiscard]]
            static std::unique_ptr<SessionCore>
            open_for_test();

            // Compose a core that owns and is driven by an externally-provided
            // runtime (set as the primary). For daemon/observation composition
            // and tests that need a controllable runtime. Does not wire event
            // sink/demand; call start_observation() for live observation.
            [[nodiscard]]
            static Result<std::unique_ptr<SessionCore>>
            open_owning( std::unique_ptr<spi::Runtime> runtime,
                         const OperationContext&       context );

            ~SessionCore();

            SessionCore( const SessionCore& ) = delete;
            SessionCore&
            operator=( const SessionCore& ) = delete;
            SessionCore( SessionCore&& )    = delete;
            SessionCore&
            operator=( SessionCore&& ) = delete;

            [[nodiscard]]
            Result<void>
            attach( spi::Runtime&            runtime,
                    const OperationContext&  context,
                    std::optional<RuntimeId> assigned_runtime = std::nullopt );

            [[nodiscard]]
            Result<Match>
            resolve( const Locator& locator,
                     Cardinality    cardinality = Cardinality::ExactlyOne );

            [[nodiscard]]
            Result<NodeInfo>
            describe( const Match& match );

            [[nodiscard]]
            Result<Subscription>
            watch( SubscriptionScope scope,
                   QueueOptions      options = {} );

            [[nodiscard]]
            Result<Receipt>
            perform( const Action&        action,
                     const ActionOptions& options = {} );

            [[nodiscard]]
            Result<Frame>
            capture( const CaptureTarget& target,
                     CaptureOptions       options = {} );

            [[nodiscard]]
            EventBus&
            bus() noexcept;

            [[nodiscard]]
            TreeStore&
            store() noexcept;

            [[nodiscard]]
            std::size_t
            store_count() const noexcept;

            [[nodiscard]]
            TreeStore*
            store_at( std::size_t index ) noexcept;

            [[nodiscard]]
            TargetRegistry&
            registry() noexcept;

            [[nodiscard]]
            const std::vector<DiagnosticEntry>&
            runtime_diagnostics() const noexcept;

            // Precondition: this core was created by open(), or a runtime was
            // attach()ed.
            [[nodiscard]]
            spi::Runtime&
            primary_runtime() noexcept;

            [[nodiscard]]
            Result<void>
            pump_once( const OperationContext& context );

            // Session-assigned runtime id for the runtime bound at store index
            // `index`, or a default RuntimeId when the index is out of range.
            [[nodiscard]]
            RuntimeId
            runtime_id_at( std::size_t index ) const noexcept;

            // Start/stop the continuous observation pump over the primary
            // runtime's event source and periodic tree-delta drains.
            [[nodiscard]]
            Result<void>
            start_observation( const OperationContext& context );

            void
            stop_observation();

        private:

            struct RuntimeBinding
            {
                    spi::Runtime*              runtime{};
                    spi::TreeSource*           source{};
                    RuntimeId                  assigned_runtime{};
                    std::unique_ptr<TreeStore> store;
                    std::uint64_t              sink_batch_revision{};
                    std::uint64_t              sink_previous_revision{};
                    std::vector<std::pair<std::uint64_t, std::uint64_t>> pending_active;
                    std::vector<std::pair<std::uint64_t, std::uint64_t>> current_active;
            };

            SessionCore();

            RuntimeBinding&
            make_binding();

            void
            publish_tree_event( RuntimeBinding&          binding,
                                const kernel::TreeEvent& event );

            void
            compose_atspi_best_effort( grab::core::Reactor*    reactor,
                                       const OperationContext& context );

            [[nodiscard]]
            Result<std::size_t>
            drain_source( spi::TreeSource&        source,
                          TreeStore&              store,
                          const OperationContext& context );

            [[nodiscard]]
            RuntimeId
                                                         allocate_runtime_id() noexcept;

            EventBus                                     bus_;
            std::vector<std::unique_ptr<RuntimeBinding>> bindings_;
            TargetRegistry                               owned_registry_;
            TargetRegistry*                              registry_{ &owned_registry_ };
            std::unique_ptr<spi::Runtime>                owned_runtime_;
            std::unique_ptr<spi::Runtime>                atspi_runtime_;
            spi::Runtime*                                primary_runtime_{};
            grab::drivers::desktop::x11::X11Runtime*     x11_runtime_{};
            std::vector<DiagnosticEntry>                 runtime_diagnostics_;

            // Monotonic session-scoped RuntimeId authority. Each attached
            // runtime is assigned a distinct, stable id via allocate_runtime_id;
            // publish_tree_event stamps it into subject.runtime so bus events
            // disambiguate multi-runtime output regardless of a source's own
            // restart-scoped id. Source-minted ids still drive per-store restart
            // detection.
            std::uint32_t                                next_runtime_id_{ 1U };
            std::unique_ptr<ObservationPump>             pump_;
    };

    [[nodiscard]]
    Result<Match>
    resolve_verb( SessionCore*   core,
                  const Locator& locator,
                  Cardinality    cardinality );

    [[nodiscard]]
    Result<NodeInfo>
    describe_verb( SessionCore* core,
                   const Match& match );

    [[nodiscard]]
    Result<Subscription>
    watch_verb( SessionCore*      core,
                SubscriptionScope scope,
                QueueOptions      options );

    [[nodiscard]]
    Result<Receipt>
    perform_verb( SessionCore*         core,
                  const Action&        action,
                  const ActionOptions& options );

    [[nodiscard]]
    Result<Frame>
    capture_verb( SessionCore*         core,
                  const CaptureTarget& target,
                  CaptureOptions       options );

}    // namespace grab::kernel::lifecycle
