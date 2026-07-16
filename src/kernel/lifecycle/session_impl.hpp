#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/capture.hpp"
#include "grab/context.hpp"
#include "grab/event_bus.hpp"
#include "grab/interaction.hpp"
#include "grab/locator.hpp"
#include "grab/query.hpp"
#include "grab/result.hpp"
#include "grab/session.hpp"
#include "grab/trace.hpp"
#include "kernel/graph/target_registry.hpp"
#include "kernel/graph/tree_store.hpp"
#include "spi/runtime.hpp"
#include "spi/tree_source.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
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

            ~SessionCore();

            SessionCore( const SessionCore& ) = delete;
            SessionCore&
            operator=( const SessionCore& ) = delete;
            SessionCore( SessionCore&& )    = delete;
            SessionCore&
            operator=( SessionCore&& ) = delete;

            [[nodiscard]]
            Result<void>
            attach( spi::Runtime&           runtime,
                    const OperationContext& context );

            [[nodiscard]]
            Result<Match>
            resolve( const Locator& locator,
                     Cardinality    cardinality = Cardinality::ExactlyOne );

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

        private:

            SessionCore();

            void
            publish_tree_event( const kernel::TreeEvent& event );

            void
            compose_atspi_best_effort( grab::core::Reactor*    reactor,
                                       const OperationContext& context );

            [[nodiscard]]
            Result<std::size_t>
                            drain_source( spi::TreeSource&        source,
                                          const OperationContext& context );

            EventBus        bus_;
            TreeStore       store_;
            TargetRegistry  owned_registry_;
            TargetRegistry* registry_{ &owned_registry_ };
            std::unique_ptr<spi::Runtime>            owned_runtime_;
            spi::Runtime*                            primary_runtime_{};
            grab::drivers::desktop::x11::X11Runtime* x11_runtime_{};
            std::vector<spi::TreeSource*>            attached_;
            std::vector<DiagnosticEntry>             runtime_diagnostics_;
            std::uint64_t                            sink_batch_revision_{};
            std::uint64_t                            sink_previous_revision_{};
            std::vector<std::pair<std::uint64_t, std::uint64_t>> pending_active_;
            std::vector<std::pair<std::uint64_t, std::uint64_t>> current_active_;
    };

    [[nodiscard]]
    Result<Match>
    resolve_verb( SessionCore*   core,
                  const Locator& locator,
                  Cardinality    cardinality );

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
