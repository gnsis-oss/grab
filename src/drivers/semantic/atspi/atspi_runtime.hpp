#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "drivers/semantic/atspi/atspi_tree_source.hpp"
#include "spi/runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace grab
{

    class EventBus;

}    // namespace grab

namespace grab::core
{

    class Reactor;

}    // namespace grab::core

namespace grab::event
{

    class AtspiMonitor;

}    // namespace grab::event

namespace grab::kernel
{

    class TargetRegistry;

}    // namespace grab::kernel

namespace grab::drivers::semantic::atspi
{

    class AtspiEventSource;

    class AtspiRuntime final : public grab::spi::Runtime
    {
        public:

            // Useful for discovery/probing; start() reports CapabilityUnavailable
            // until a reactor and event bus are injected.
            AtspiRuntime();

            AtspiRuntime(
                grab::core::Reactor&                  reactor,
                grab::EventBus&                       event_bus,
                AtspiTreeSource::AccessibleEnumerator enumerate_accessibles = {},
                std::optional<std::string>            x11_alias_authority = std::nullopt
            );

            AtspiRuntime(
                grab::core::Reactor&                  reactor,
                grab::EventBus&                       event_bus,
                grab::kernel::TargetRegistry&         targets,
                AtspiTreeSource::AccessibleEnumerator enumerate_accessibles = {},
                std::optional<std::string>            x11_alias_authority = std::nullopt
            );

            ~AtspiRuntime() override;

            [[nodiscard]]
            std::string_view
            name() const override;

            [[nodiscard]]
            std::uint32_t
            generation() const override;

            [[nodiscard]]
            grab::Result<void>
            start( const grab::OperationContext& context ) override;

            [[nodiscard]]
            grab::Result<void>
            stop() override;

            [[nodiscard]]
            grab::spi::TreeSource*
            tree_source() override;

            [[nodiscard]]
            grab::spi::EventSource*
            event_source() override;

            [[nodiscard]]
            std::span<const grab::spi::RouteDescriptor>
            routes() const override;

            [[nodiscard]]
            grab::spi::ActionRoute*
            action_route( std::size_t index ) override;

        private:

            static constexpr std::uint32_t                initialGeneration = 1U;

            grab::core::Reactor*                          reactor_{};
            grab::EventBus*                               event_bus_{};
            std::unique_ptr<grab::kernel::TargetRegistry> owned_targets_;
            grab::kernel::TargetRegistry*                 targets_{};
            AtspiTreeSource::AccessibleEnumerator         enumerate_accessibles_;
            std::optional<std::string>                    x11_alias_authority_;
            std::unique_ptr<grab::event::AtspiMonitor>    monitor_;
            std::unique_ptr<AtspiTreeSource>              tree_source_;
            std::unique_ptr<AtspiEventSource>             event_source_;
            std::uint32_t generation_{ initialGeneration };
            bool          has_started_{};
    };

}    // namespace grab::drivers::semantic::atspi
