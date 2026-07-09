#pragma once

#include "grab/result.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

struct xkb_context;
struct xkb_keymap;

namespace grab::input
{

    struct Keystroke
    {
            std::uint8_t keycode = 0U;
            bool         shift   = false;
            bool         level3  = false;
    };

    class Keymap
    {
        public:

            [[nodiscard]]
            static grab::Result<Keymap>
            open_layout( std::string_view layout = "us" );

            ~Keymap();

            Keymap( const Keymap& ) = delete;
            Keymap&
            operator=( const Keymap& ) = delete;
            Keymap( Keymap&& other ) noexcept;
            Keymap&
            operator=( Keymap&& other ) noexcept;

            [[nodiscard]]
            grab::Result<std::vector<Keystroke>>
            text_to_keystrokes( std::string_view utf8 ) const;

            [[nodiscard]]
            grab::Result<Keystroke>
            codepoint_to_keystroke( char32_t codepoint ) const;

            [[nodiscard]]
            std::uint8_t
            shift_keycode() const;

            [[nodiscard]]
            std::uint8_t
            level3_keycode() const;

        private:

            Keymap( xkb_context* context,
                    xkb_keymap*  keymap ) noexcept;

            xkb_context* context_ = nullptr;
            xkb_keymap*  keymap_  = nullptr;
    };

}    // namespace grab::input
