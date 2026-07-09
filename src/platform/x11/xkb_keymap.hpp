#pragma once

#include "grab/keymap.hpp"

#include <string_view>

namespace grab::platform::x11
{

    class XcbConnection;

    [[nodiscard]]
    grab::Result<grab::Keymap>
    make_keymap_from_layout( std::string_view layout = "us" );

    [[nodiscard]]
    grab::Result<grab::Keymap>
    make_keymap_from_connection( const XcbConnection& conn );

}    // namespace grab::platform::x11
