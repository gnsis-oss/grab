#pragma once

#include "grab/result.hpp"

#include <optional>
#include <string>
#include <xcb/xcb.h>

namespace grab::session
{

    // Display numbers grab is willing to claim for a display it starts. Low
    // numbers belong to whoever is logged in.
    inline constexpr int firstProvisionableDisplay = 100;
    inline constexpr int lastProvisionableDisplay  = 199;

    [[nodiscard]]
    std::string
    display_name_for( int display_number );

    // True when an X server answers on `display`. Connect-and-disconnect, so a
    // server started without -noreset resets on the disconnect; every server
    // grab starts passes -noreset for exactly that reason.
    [[nodiscard]]
    bool
    display_connectable( const std::string& display );

    // A display number with neither a lock file, a socket, nor a server
    // answering on it. Racy by nature — two callers scanning at once see the
    // same free number — so a caller that spawns a server must confirm its own
    // child is the one that came up.
    [[nodiscard]]
    grab::Result<std::string>
    find_free_display();

    // A held connection that answers "is this precondition actually satisfied"
    // without spawning anything. The questions are the ones grab's own drivers
    // ask before they agree to work, asked in the same way:
    // _NET_SUPPORTING_WM_CHECK / the ICCCM manager selection for focus,
    // _NET_WM_CM_S<n> ownership for the overlay, and the AT_SPI_BUS root
    // property for the accessibility tree.
    class DisplayProbe
    {
        public:

            [[nodiscard]]
            static grab::Result<DisplayProbe>
            open( const std::string& display );

            ~DisplayProbe();

            DisplayProbe( const DisplayProbe& ) = delete;
            DisplayProbe&
            operator=( const DisplayProbe& ) = delete;
            DisplayProbe( DisplayProbe&& other ) noexcept;
            DisplayProbe&
            operator=( DisplayProbe&& other ) noexcept;

            // False once the server has gone away underneath us.
            [[nodiscard]]
            bool
            connected() const noexcept;

            [[nodiscard]]
            bool
            window_manager_present() const;

            [[nodiscard]]
            bool
            compositor_present() const;

            // The accessibility bus address advertised on the root window, as
            // an AT-SPI client on X11 discovers it.
            [[nodiscard]]
            std::optional<std::string>
            accessibility_bus_address() const;

            // "_NET_WM_CM_S0" — named in reason strings so a reader can check
            // the same thing by hand.
            [[nodiscard]]
            std::string
            compositor_selection_name() const;

        private:

            DisplayProbe( xcb_connection_t* connection,
                          xcb_window_t      root,
                          int               screen_index ) noexcept;

            void
            close() noexcept;

            [[nodiscard]]
            xcb_atom_t
            existing_atom( const std::string& name ) const;

            [[nodiscard]]
            bool
                              selection_owned( const std::string& name ) const;

            // Held open for the lifetime of the probe: every query is asked
            // of the live server, never of a remembered answer.
            xcb_connection_t* connection_   = nullptr;
            xcb_window_t      root_         = XCB_NONE;
            int               screen_index_ = 0;
    };

}    // namespace grab::session
