#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "drivers/desktop/x11/injection_ledger.hpp"
#include "drivers/desktop/x11/x11_capture_route.hpp"
#include "drivers/desktop/x11/xcb_connection.hpp"
#include "grab/capability.hpp"
#include "grab/event.hpp"
#include "grab/ids.hpp"
#include "grab/result.hpp"
#include "kernel/graph/target_registry.hpp"
#include "spi/runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace grab::core
{

    class Reactor;

}

namespace grab::drivers::desktop::x11
{

    class X11TreeSource;
    class X11EventSource;
    class X11TopologySource;
    class X11InputSeat;
    class X11KeyboardRoute;
    class X11PointerRoute;
    class X11ActivationRoute;
    class X11OverlayDelegate;

    class X11Runtime final : public grab::spi::Runtime
    {
        public:

            // `display` is the X display to connect to. Empty means DISPLAY, as
            // xcb_connect already defines it. Honouring it is not cosmetic: a
            // session that silently connects somewhere other than where the
            // caller asked draws its overlay on a display the caller never
            // named, which on a shared machine is somebody else's screen.
            explicit X11Runtime( grab::core::Reactor* reactor = nullptr,
                                 std::string          display = {} ) noexcept;
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
            X11CaptureRoute*
            capture_route() noexcept;

            [[nodiscard]]
            const grab::Error*
            capture_route_error() const noexcept;

            void
            set_event_sink( std::function<void( grab::Event&& )> sink );

            [[nodiscard]]
            X11InputSeat*
            native_seat() noexcept;

            [[nodiscard]]
            grab::spi::TreeSource*
            tree_source() override;

            [[nodiscard]]
            grab::Result<std::uint32_t>
            resolve_native_window( const grab::WidgetRef& widget ) const;

            [[nodiscard]]
            grab::kernel::TargetRegistry*
            target_registry() noexcept;

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
            grab::spi::OverlayDelegate*
            overlay_delegate() override;

            [[nodiscard]]
            std::span<const grab::Capability>
            capabilities() const noexcept;

            [[nodiscard]]
            const grab::Error*
            overlay_delegate_error() const noexcept;

            [[nodiscard]]
            std::span<const grab::spi::RouteDescriptor>
            routes() const override;

            [[nodiscard]]
            grab::spi::ActionRoute*
            action_route( std::size_t index ) override;

            [[nodiscard]]
            grab::spi::InputSeat*
            input_seat() override;

        private:

            [[nodiscard]]
            const char*
                                                 display_or_default() const noexcept;

            static constexpr std::uint32_t       initialGeneration = 1U;

            grab::platform::x11::XcbConnection   connection_;
            grab::kernel::TargetRegistry         targets_;
            std::unique_ptr<X11TreeSource>       tree_source_;
            std::unique_ptr<X11InputSeat>        input_seat_;
            std::unique_ptr<X11PointerRoute>     pointer_route_;
            std::unique_ptr<X11KeyboardRoute>    keyboard_route_;
            std::unique_ptr<X11ActivationRoute>  activation_route_;
            std::optional<X11CaptureRoute>       capture_route_;
            std::optional<grab::Error>           capture_route_error_;
            InjectionLedger                      ledger_;
            std::unique_ptr<X11EventSource>      event_source_;
            std::unique_ptr<X11TopologySource>   topology_source_;
            std::unique_ptr<X11OverlayDelegate>  overlay_delegate_;
            std::optional<grab::Error>           overlay_delegate_error_;
            std::function<void( grab::Event&& )> pending_sink_;
            std::string                          display_{};
            grab::core::Reactor*                 reactor_{};
            std::uint32_t                        generation_{ initialGeneration };
            bool                                 overlay_available_{};
            bool                                 has_started_{};
    };

}    // namespace grab::drivers::desktop::x11
