#pragma once

#include "grab/result.hpp"

#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <xkbcommon/xkbcommon.h>

namespace grab::platform::x11
{

    class XcbConnection;

    class XkbKeymap
    {
        public:

            struct KeyStroke
            {
                    xkb_keycode_t keycode     = XKB_KEYCODE_INVALID;
                    bool          needs_shift = false;

                    [[nodiscard]]
                    friend bool
                    operator==( const KeyStroke& lhs,
                                const KeyStroke& rhs ) = default;
            };

            [[nodiscard]]
            static grab::Result<XkbKeymap>
            from_connection( const XcbConnection& conn );

            [[nodiscard]]
            static grab::Result<XkbKeymap>
            from_default_names();

            XkbKeymap( const XkbKeymap& ) = delete;
            XkbKeymap( XkbKeymap&& other ) noexcept;

            XkbKeymap&
            operator=( const XkbKeymap& ) = delete;

            XkbKeymap&
            operator=( XkbKeymap&& other ) noexcept;

            ~XkbKeymap();

            [[nodiscard]]
            std::optional<KeyStroke>
            stroke_for_keysym( xkb_keysym_t keysym ) const;

            [[nodiscard]]
            std::optional<xkb_keysym_t>
            keysym_for_stroke( const KeyStroke& stroke ) const;

            [[nodiscard]]
            grab::Result<std::vector<KeyStroke>>
            strokes_for_text( std::string_view utf8 ) const;

            [[nodiscard]]
            static std::optional<xkb_keysym_t>
            keysym_from_name( std::string_view name );

        private:

            XkbKeymap( xkb_context* context,
                       xkb_keymap*  keymap,
                       xkb_state*   state );

            void
            build_reverse_lookup();

            void
                         record_keysyms( xkb_keycode_t     keycode,
                                         xkb_level_index_t level );

            xkb_context* context = nullptr;
            xkb_keymap*  keymap  = nullptr;
            xkb_state*   state   = nullptr;
            std::unordered_map<xkb_keysym_t, KeyStroke> reverse;
    };

}    // namespace grab::platform::x11
