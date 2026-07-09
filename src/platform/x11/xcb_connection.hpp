#pragma once

#include "grab/result.hpp"

#include <string_view>
#include <xcb/xcb.h>

namespace grab::platform::x11
{

    class XcbConnection
    {
        public:

            [[nodiscard]]
            static grab::Result<XcbConnection>
            open( std::string_view display );

            XcbConnection()                       = default;
            XcbConnection( const XcbConnection& ) = delete;
            XcbConnection( XcbConnection&& other ) noexcept;

            XcbConnection&
            operator=( const XcbConnection& ) = delete;

            XcbConnection&
            operator=( XcbConnection&& other ) noexcept;

            ~XcbConnection();

            [[nodiscard]]
            xcb_connection_t*
            get() const noexcept;

            [[nodiscard]]
            xcb_window_t
            root() const noexcept;

            [[nodiscard]]
            bool
            has_shm() const noexcept;

            [[nodiscard]]
            bool
            has_xfixes() const noexcept;

        private:

            XcbConnection( xcb_connection_t* connection,
                           xcb_window_t      root,
                           bool              has_shm,
                           bool              has_xfixes ) noexcept;

            xcb_connection_t* connection           = nullptr;
            xcb_window_t      root_window          = XCB_NONE;
            bool              has_shm_extension    = false;
            bool              has_xfixes_extension = false;
    };

}    // namespace grab::platform::x11
