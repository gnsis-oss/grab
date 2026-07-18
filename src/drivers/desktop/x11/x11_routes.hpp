#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "drivers/desktop/x11/injection_ledger.hpp"
#include "drivers/desktop/x11/x11_input_correctness.hpp"
#include "drivers/desktop/x11/x11_xtest_seat.hpp"
#include "grab/capability.hpp"
#include "grab/keymap.hpp"
#include "spi/event_source.hpp"
#include "spi/route.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <xcb/xcb.h>

namespace grab::drivers::desktop::x11
{

    class X11TreeSource;

    [[nodiscard]]
    std::span<const grab::Capability>
    x11_capability_rows( bool overlay_available ) noexcept;

    class X11InputSeat final : public grab::spi::InputSeat,
                               public ModifierState
    {
        public:

            explicit X11InputSeat( grab::input::Seat seat,
                                   InjectionLedger*  ledger = nullptr ) noexcept;

            [[nodiscard]]
            grab::Result<void>
            move_pointer_absolute( std::int16_t x,
                                   std::int16_t y );

            [[nodiscard]]
            grab::Result<void>
            button( std::uint8_t button,
                    bool         press );

            [[nodiscard]]
            grab::Result<void>
            key( std::uint8_t keycode,
                 bool         press );

            [[nodiscard]]
            grab::Result<void>
            flush();

            [[nodiscard]]
            SeatLane::Token
            acquire_lane();

            [[nodiscard]]
            bool
            held( std::uint8_t keycode ) const override;

            [[nodiscard]]
            bool
            set( std::uint8_t keycode,
                 bool         press ) override;

            [[nodiscard]]
            grab::Result<grab::NeutralizationOutcome>
            neutralize( const grab::OperationContext& context ) override;

        private:

            grab::input::Seat      seat_;
            InjectionLedger*       ledger_{};
            std::array<bool, 256U> held_buttons_{};
            std::array<bool, 256U> held_keys_{};
            mutable std::mutex     mutex_;
            SeatLane               lane_;
    };

    class X11PointerRoute final : public grab::spi::ActionRoute
    {
        public:

            X11PointerRoute( X11TreeSource&    source,
                             xcb_connection_t* connection,
                             xcb_window_t      root,
                             X11InputSeat&     seat ) noexcept;

            [[nodiscard]]
            grab::Result<std::unique_ptr<grab::spi::RouteReservation>>
            reserve( const grab::spi::ActionRequest& action,
                     const grab::OperationContext&   context ) override;

        private:

            X11TreeSource*    source_{};
            xcb_connection_t* connection_{};
            xcb_window_t      root_{};
            X11InputSeat*     seat_{};
    };

    class X11KeyboardRoute final : public grab::spi::ActionRoute
    {
        public:

            X11KeyboardRoute( X11TreeSource&    source,
                              xcb_connection_t* connection,
                              X11InputSeat&     seat,
                              grab::Keymap      keymap ) noexcept;

            [[nodiscard]]
            grab::Result<std::unique_ptr<grab::spi::RouteReservation>>
            reserve( const grab::spi::ActionRequest& action,
                     const grab::OperationContext&   context ) override;

        private:

            X11TreeSource*     source_{};
            xcb_connection_t*  connection_{};
            X11InputSeat*      seat_{};
            grab::Keymap       keymap_;
            ScratchKeycodePool scratch_pool_;
    };

    class X11ActivationRoute final : public grab::spi::ActionRoute
    {
        public:

            X11ActivationRoute( X11TreeSource&          source,
                                xcb_connection_t*       connection,
                                xcb_window_t            root,
                                grab::spi::EventSource& events ) noexcept;

            [[nodiscard]]
            grab::Result<std::unique_ptr<grab::spi::RouteReservation>>
            reserve( const grab::spi::ActionRequest& action,
                     const grab::OperationContext&   context ) override;

        private:

            X11TreeSource*          source_{};
            xcb_connection_t*       connection_{};
            xcb_window_t            root_{};
            grab::spi::EventSource* events_{};
    };

    [[nodiscard]]
    inline std::span<const grab::spi::RouteDescriptor>
    x11_route_descriptors() noexcept
    {
        static constexpr std::array pointer_constraints{
            grab::spi::RouteConstraint{ "display", "X11 XTEST extension" },
        };
        static constexpr std::array keyboard_constraints{
            grab::spi::RouteConstraint{ "display", "X11 XTEST and XKB" },
        };
        static constexpr std::array capture_constraints{
            grab::spi::RouteConstraint{ "display", "mapped X11 window" },
        };
        static constexpr std::array activation_constraints{
            grab::spi::RouteConstraint{ "display", "EWMH _NET_ACTIVE_WINDOW" },
        };
        static constexpr std::array descriptors{
            grab::spi::RouteDescriptor{
                                       "x11.pointer", grab::spi::RouteKind::Physical,
                                       grab::spi::RouteFidelity::Exact,
                                       grab::spi::RouteLatencyClass::Immediate,
                                       pointer_constraints, },
            grab::spi::RouteDescriptor{
                                       "x11.keyboard", grab::spi::RouteKind::Physical,
                                       grab::spi::RouteFidelity::Lossless,
                                       grab::spi::RouteLatencyClass::Immediate,
                                       keyboard_constraints, },
            grab::spi::RouteDescriptor{
                                       "x11.capture", grab::spi::RouteKind::Physical,
                                       grab::spi::RouteFidelity::Exact,
                                       grab::spi::RouteLatencyClass::Interactive,
                                       capture_constraints, },
            grab::spi::RouteDescriptor{
                                       "x11.activate", grab::spi::RouteKind::Physical,
                                       grab::spi::RouteFidelity::Exact,
                                       grab::spi::RouteLatencyClass::Interactive,
                                       activation_constraints, },
        };
        return descriptors;
    }

}    // namespace grab::drivers::desktop::x11
