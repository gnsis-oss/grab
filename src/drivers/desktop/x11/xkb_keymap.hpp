#pragma once

#include "grab/keymap.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

struct xcb_connection_t;

namespace grab::platform::x11
{

    class XcbConnection;

    class XkbKeymapSnapshot final
    {
        public:

            XkbKeymapSnapshot( const XkbKeymapSnapshot& ) = delete;
            XkbKeymapSnapshot( XkbKeymapSnapshot&& ) noexcept;

            XkbKeymapSnapshot&
            operator=( const XkbKeymapSnapshot& ) = delete;

            XkbKeymapSnapshot&
            operator=( XkbKeymapSnapshot&& ) noexcept;

            ~XkbKeymapSnapshot();

            [[nodiscard]]
            std::string
            base_key_name( std::uint32_t keycode ) const;

        private:

            class Impl;

            explicit XkbKeymapSnapshot( std::unique_ptr<Impl> impl ) noexcept;

            std::unique_ptr<Impl> impl_;

            friend grab::Result<XkbKeymapSnapshot>
            make_keymap_from_connection( xcb_connection_t* connection );
    };

    [[nodiscard]]
    grab::Result<grab::Keymap>
    make_keymap_from_layout( std::string_view layout );

    [[nodiscard]]
    grab::Result<XkbKeymapSnapshot>
    make_keymap_from_connection( xcb_connection_t* connection );

    [[nodiscard]]
    grab::Result<grab::Keymap>
    make_keymap_from_connection( const XcbConnection& conn );

}    // namespace grab::platform::x11
