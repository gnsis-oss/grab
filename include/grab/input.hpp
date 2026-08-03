#pragma once

#include "grab/drag.hpp"
#include "grab/geometry.hpp"
#include "grab/geometry/point.hpp"
#include "grab/pointer_button.hpp"
#include "grab/result.hpp"

#include <cstdint>
#include <memory>
#include <string_view>

namespace grab::input
{

    using grab::geometry::Point;

}    // namespace grab::input

namespace grab
{

    class Input
    {
        public:

            [[nodiscard]]
            static grab::Result<Input>
            open( const char*      display = nullptr,
                  std::string_view layout  = {} );

            ~Input();

            Input( const Input& ) = delete;
            Input&
            operator=( const Input& ) = delete;
            Input( Input&& other ) noexcept;
            Input&
            operator=( Input&& other ) noexcept;

            [[nodiscard]]
            grab::Result<grab::geometry::Point>
            position();

            [[nodiscard]]
            grab::Result<void>
            move( std::int16_t x,
                  std::int16_t y );

            [[nodiscard]]
            grab::Result<void>
            click( std::uint8_t button = grab::input::primaryButton );

            // Press and release, separately.
            //
            // click() holds the button for however long two XTEST requests take,
            // which is both constant and far shorter than a human's 50-150 ms.
            // A caller that wants a realistic hold, or a drag, needs the two
            // halves; the seat has always had them and only the facade did not.
            //
            // The caller owns what it presses: a button left down survives the
            // Input object and is still down for the next application to receive
            // it. Pair every press with a release on every exit path.
            [[nodiscard]]
            grab::Result<void>
            press( std::uint8_t button = grab::input::primaryButton );

            [[nodiscard]]
            grab::Result<void>
            release( std::uint8_t button = grab::input::primaryButton );

            // Wheel motion, in notches. Positive dy scrolls DOWN and positive dx
            // scrolls RIGHT, matching how a wheel is described rather than how
            // the content moves. X11 has no sub-notch wheel event, so this is
            // deliberately integral: a caller asking for smooth pixel scrolling
            // is asking for something the protocol cannot express.
            [[nodiscard]]
            grab::Result<void>
            scroll( std::int32_t dx,
                    std::int32_t dy );

            [[nodiscard]]
            grab::Result<void>
            click_at( std::int16_t x,
                      std::int16_t y,
                      std::uint8_t button = grab::input::primaryButton );

            [[nodiscard]]
            grab::Result<void>
            drag( grab::input::Point              from,
                  grab::input::Point              to,
                  const grab::input::DragOptions& options = {} );

            [[nodiscard]]
            grab::Result<void>
            type_text( std::string_view utf8 );

            [[nodiscard]]
            grab::Result<void>
            press_key( std::string_view name );

            // Hold and release a named key, so callers can build chords the
            // keymap cannot express on its own.
            //
            // Keystroke carries only shift and altgr, because those are the
            // levels a LAYOUT uses to produce a character. Ctrl, Alt and Super
            // change what an application does with a character rather than which
            // character it is, so they are not layout state and cannot be
            // reached through type_text or press_key. Ctrl+C is therefore:
            //
            //     key_down( "Control_L" ); press_key( "c" ); key_up( "Control_L" );
            //
            // Any name xkb_keysym_from_name accepts works, modifiers included.
            // As with press(), a key left down stays down — release on every
            // exit path, including the error ones.
            [[nodiscard]]
            grab::Result<void>
            key_down( std::string_view name );

            [[nodiscard]]
            grab::Result<void>
            key_up( std::string_view name );

        private:

            class Impl;

            explicit Input( std::unique_ptr<Impl> impl ) noexcept;

            [[nodiscard]]
            grab::Result<Impl*>
                                  require_impl() noexcept;

            std::unique_ptr<Impl> impl_;
    };

}    // namespace grab
