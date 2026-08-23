#pragma once

#include "grab/result.hpp"
#include "grab/session.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace grab
{

    enum class DisplayBackend : std::uint8_t
    {
        Headless,    // Xvfb — no host display needed
        Nested,      // Xephyr — a window on an existing display
        Existing,    // attach to a display already running (nothing is spawned)
    };

    inline constexpr std::uint16_t defaultDisplayWidth  = 1'280U;
    inline constexpr std::uint16_t defaultDisplayHeight = 900U;

    // What to provision. The three precondition flags default ON because a
    // display without them is one grab cannot honestly drive: a click lands
    // but activates nothing without a window manager, an overlay draws
    // nothing without a compositing manager, and resolve/describe see nothing
    // without an accessibility bus. A caller attaching to a desktop that
    // already has them turns them off.
    struct DisplayRequest
    {
            DisplayBackend             backend = DisplayBackend::Headless;
            // Empty means grab picks a free display number.
            std::optional<std::string> display;
            std::uint16_t              width  = defaultDisplayWidth;
            std::uint16_t              height = defaultDisplayHeight;
            // Nested: where the window opens. Empty means the ambient DISPLAY.
            std::optional<std::string> host_display;

            bool                       window_manager = true;
            bool                       compositor     = true;
            // The session bus is the prerequisite of the accessibility bus, so
            // requesting the latter without the former is rejected.
            bool                       session_bus       = true;
            bool                       accessibility_bus = true;
    };

    // A display grab provisioned and owns. Destruction tears down exactly the
    // processes this object started, youngest first, by recorded pidfd —
    // never by process name, which on a shared machine reaches other people's
    // processes.
    class ProvisionedDisplay
    {
        public:

            ~ProvisionedDisplay();

            ProvisionedDisplay( const ProvisionedDisplay& ) = delete;
            ProvisionedDisplay&
            operator=( const ProvisionedDisplay& ) = delete;
            ProvisionedDisplay( ProvisionedDisplay&& ) noexcept;
            ProvisionedDisplay&
            operator=( ProvisionedDisplay&& ) noexcept;

            // ":77". Feed it to SessionOptions::display, or use open_session()
            // below to remove the last chance to mismatch the two.
            [[nodiscard]]
            std::string_view
            name() const noexcept;

            // Environment a child process must inherit to land on this display
            // and reach its accessibility bus (DISPLAY,
            // DBUS_SESSION_BUS_ADDRESS, the AT-SPI bridge switches). The
            // consumer still launches its own application; it should not have
            // to know which variables matter.
            [[nodiscard]]
            std::vector<std::pair<std::string,
                                  std::string>>
            child_environment() const;

            // Apply child_environment() to THIS process. grab's own AT-SPI
            // driver resolves the accessibility bus through the session bus
            // named by this process's DBUS_SESSION_BUS_ADDRESS, so a session
            // opened on a provisioned display resolves nothing until either
            // this is called or the caller exports the same variables itself.
            // Not thread-safe: it calls setenv, so call it before any thread
            // that reads the environment starts.
            [[nodiscard]]
            Result<void>
            adopt_environment() const;

            // Which preconditions are actually live, right now, each failure
            // carrying grab's usual CapabilityUnavailable reason string. The
            // answer is probed, not remembered: the same query answers for a
            // display grab started and for one it attached to, and it notices
            // a service that has since died.
            [[nodiscard]]
            Result<void>
            window_manager() const;

            [[nodiscard]]
            Result<void>
            compositor() const;

            [[nodiscard]]
            Result<void>
            accessibility() const;

        private:

            class Impl;

            explicit ProvisionedDisplay( std::unique_ptr<Impl> impl ) noexcept;

            friend Result<ProvisionedDisplay>
                                  provision_display( DisplayRequest request );

            std::unique_ptr<Impl> impl_;
    };

    // Provision a display with the preconditions grab needs already satisfied.
    //
    // Ordering is not the caller's problem: the display accepts connections
    // before the window manager starts, the session bus exists before the
    // accessibility bus, and the accessibility bus is up before this returns —
    // so an AT-SPI-reading application launched into child_environment() finds
    // it.
    //
    // Headless and Nested fail, loudly and with a reason naming what to
    // install, when a requested precondition cannot be started. Existing
    // spawns nothing at all — it is the "drive the display the operator is
    // already using" path — so it reports its preconditions rather than
    // starting them.
    [[nodiscard]]
    Result<ProvisionedDisplay>
    provision_display( DisplayRequest request );

    // Session::open against a provisioned display, with the display name taken
    // from the display itself rather than copied by the caller.
    //
    // This calls adopt_environment() first, because a session opened on a
    // provisioned display whose accessibility bus this process cannot see is
    // exactly the silent failure the provisioning API exists to remove. Use
    // Session::open({ .display = display.name() }) directly when the process
    // environment must not be touched.
    [[nodiscard]]
    Result<std::unique_ptr<Session>>
    open_session( const ProvisionedDisplay& display,
                  SessionOptions            options = {} );

}    // namespace grab
