#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "platform/x11/xcb_connection.hpp"
#include "spi/runtime.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace grab::kernel
{

    class TargetRegistry;

}

namespace grab::drivers::desktop::x11
{

    class X11TreeSource;

    class X11Runtime final : public grab::spi::Runtime
    {
        public:

            X11Runtime();
            ~X11Runtime() override;

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
            const grab::kernel::TargetRegistry*
            target_registry() const noexcept;

            [[nodiscard]]
            grab::spi::TopologySource*
            topology_source() override;

            [[nodiscard]]
            grab::spi::EventSource*
            event_source() override;

            [[nodiscard]]
            std::span<const grab::spi::RouteDescriptor>
            routes() const override;

        private:

            static constexpr std::uint32_t     initialGeneration = 1U;

            grab::platform::x11::XcbConnection connection_;
            std::unique_ptr<X11TreeSource>     tree_source_;
            std::uint32_t                      generation_{ initialGeneration };
            bool                               has_started_{};
    };

}    // namespace grab::drivers::desktop::x11
