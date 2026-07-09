#pragma once

#include "grab/keymap.hpp"
#include "grab/window.hpp"
#include "input/input_sink.hpp"

#include <cstdint>
#include <string_view>
#include <vector>
#include <xkbcommon/xkbcommon.h>

namespace grab::platform::x11
{

    class XcbConnection;

    class XtestInputSink final : public grab::input::InputSink
    {
        public:

            XtestInputSink( const XcbConnection& conn,
                            const grab::Keymap&  keymap,
                            grab::WindowRef      target ) noexcept;

            void
            move( grab::input::Point p ) override;

            void
            button( std::uint8_t code,
                    bool         press,
                    bool         clear_modifiers ) override;

            void
            sync() override;

            void
            wait( std::uint32_t millis ) override;

            void
            type_text( std::string_view utf8 ) override;

            void
            key( std::string_view keysym ) override;

            void
            activate() override;

        private:

            void
            emit_key( std::uint8_t  type,
                      xkb_keycode_t keycode );

            void
            emit_button( std::uint8_t code,
                         bool         press );

            void
            press_modifier( xkb_keycode_t keycode );

            void
            release_modifier( xkb_keycode_t keycode );

            void
            release_held_modifiers();

            void
            press_stroke( const grab::Keystroke& stroke );

            void
                                       flush();

            const XcbConnection&       conn;
            const grab::Keymap&        keymap;
            grab::WindowRef            target;
            std::vector<xkb_keycode_t> held_modifiers;
    };

}    // namespace grab::platform::x11
