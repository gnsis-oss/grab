#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/context.hpp"
#include "grab/event_bus.hpp"
#include "grab/result.hpp"
#include "grab/session.hpp"
#include "kernel/graph/target_registry.hpp"
#include "kernel/graph/tree_store.hpp"
#include "spi/runtime.hpp"
#include "spi/tree_source.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace grab::kernel::lifecycle
{

    class SessionCore
    {
        public:

            [[nodiscard]]
            static Result<std::unique_ptr<SessionCore>>
            open( const SessionOptions& options );

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
            EventBus&
            bus() noexcept;

            [[nodiscard]]
            TreeStore&
            store() noexcept;

            [[nodiscard]]
            TargetRegistry&
            registry() noexcept;

            // Precondition: this core was created by open(), not open_for_test().
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

            [[nodiscard]]
            Result<std::size_t>
                            drain_source( spi::TreeSource&        source,
                                          const OperationContext& context );

            EventBus        bus_;
            TreeStore       store_;
            TargetRegistry  owned_registry_;
            TargetRegistry* registry_{ &owned_registry_ };
            std::unique_ptr<spi::Runtime> primary_runtime_;
            std::vector<spi::TreeSource*> attached_;
            std::uint64_t                 sink_batch_revision_{};
            std::uint64_t                 sink_previous_revision_{};
            std::vector<std::pair<std::uint64_t, std::uint64_t>> pending_active_;
            std::vector<std::pair<std::uint64_t, std::uint64_t>> current_active_;
    };

}    // namespace grab::kernel::lifecycle
