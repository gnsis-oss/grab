#pragma once

#include "grab/result.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace grab
{

    // A single physical key press plus the modifier levels needed to produce a
    // character, independent of any windowing system or input library.
    struct Keystroke
    {
            std::uint32_t keycode = 0U;
            bool          shift   = false;    // second-level modifier (Shift)
            bool          altgr   = false;    // third-level modifier (AltGr)

            [[nodiscard]]
            friend constexpr bool
            operator==( const Keystroke&,
                        const Keystroke& ) noexcept = default;
    };

    // Source-independent keyboard map: resolves characters and named keys to the
    // keystrokes that produce them under some layout. The generic layer knows
    // nothing about any windowing system or input library; a concrete keymap is
    // supplied by implementing Keymap::Backend and injecting it (see the platform
    // layer for the factories that build one).
    class Keymap
    {
        public:

            // The contract a specific keymap implements.
            class Backend
            {
                public:

                    Backend()                 = default;
                    virtual ~Backend()        = default;

                    Backend( const Backend& ) = delete;
                    Backend&
                    operator=( const Backend& ) = delete;
                    Backend( Backend&& )        = delete;
                    Backend&
                    operator=( Backend&& ) = delete;

                    [[nodiscard]]
                    virtual grab::Result<std::vector<Keystroke>>
                    text_to_keystrokes( std::string_view utf8 ) const = 0;

                    [[nodiscard]]
                    virtual grab::Result<Keystroke>
                    codepoint_to_keystroke( char32_t codepoint ) const = 0;

                    // Named keys such as "Return", "Escape", "Tab", "F1".
                    [[nodiscard]]
                    virtual std::optional<Keystroke>
                    keystroke_for_key( std::string_view name ) const = 0;

                    [[nodiscard]]
                    virtual std::uint32_t
                    shift_keycode() const = 0;

                    [[nodiscard]]
                    virtual std::uint32_t
                    altgr_keycode() const = 0;
            };

            explicit Keymap( std::unique_ptr<Backend> backend ) noexcept :
                backend_( std::move( backend ) )
            {
            }

            [[nodiscard]]
            grab::Result<std::vector<Keystroke>>
            text_to_keystrokes( std::string_view utf8 ) const
            {
                return backend_->text_to_keystrokes( utf8 );
            }

            [[nodiscard]]
            grab::Result<Keystroke>
            codepoint_to_keystroke( char32_t codepoint ) const
            {
                return backend_->codepoint_to_keystroke( codepoint );
            }

            [[nodiscard]]
            std::optional<Keystroke>
            keystroke_for_key( std::string_view name ) const
            {
                return backend_->keystroke_for_key( name );
            }

            [[nodiscard]]
            std::uint32_t
            shift_keycode() const
            {
                return backend_->shift_keycode();
            }

            [[nodiscard]]
            std::uint32_t
            altgr_keycode() const
            {
                return backend_->altgr_keycode();
            }

        private:

            std::unique_ptr<Backend> backend_;
    };

}    // namespace grab
