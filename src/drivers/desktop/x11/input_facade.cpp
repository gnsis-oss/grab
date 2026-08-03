#include "drivers/desktop/x11/x11_drag_recipe.hpp"
#include "drivers/desktop/x11/x11_xtest_seat.hpp"
#include "drivers/desktop/x11/xcb_connection.hpp"
#include "drivers/desktop/x11/xkb_keymap.hpp"
#include "grab/drag.hpp"
#include "grab/input.hpp"
#include "grab/keymap.hpp"
#include "grab/result.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace grab
{

    class Input::Impl
    {
        public:

            Impl( grab::input::Seat           seat,
                  std::optional<grab::Keymap> keymap,
                  std::string                 display,
                  bool                        server_keymap ) noexcept :
                seat_( std::move( seat ) ),
                keymap_( std::move( keymap ) ),
                display_( std::move( display ) ),
                server_keymap_( server_keymap )
            {
            }

            [[nodiscard]]
            grab::Result<grab::Keymap*>
            ensure_keymap();

        private:

            friend class Input;

            grab::input::Seat           seat_;
            std::optional<grab::Keymap> keymap_;
            std::string                 display_;
            bool                        server_keymap_ = false;
    };

    namespace
    {

        constexpr std::size_t maximumModifierCount = 2U;

        [[nodiscard]]
        grab::Result<void>
        error_from( grab::Result<void>& result )
        {
            return std::unexpected( std::move( result.error() ) );
        }

        [[nodiscard]]
        grab::Result<void>
        release_modifiers( grab::input::Seat&                      seat,
                           const std::array<std::uint8_t,
                                            maximumModifierCount>& modifiers,
                           std::size_t                             modifier_count )
        {
            grab::Result<void> first_error{};
            bool               has_error = false;
            for( std::size_t index = modifier_count; index > 0U; --index )
            {
                auto release_result = seat.key( modifiers.at( index - 1U ), false );
                if( !release_result.has_value() && !has_error )
                {
                    first_error = error_from( release_result );
                    has_error   = true;
                }
            }

            if( has_error )
            {
                return first_error;
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<std::uint8_t>
        xtest_keycode( std::uint32_t keycode )
        {
            if( keycode == 0U )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "XTEST keycode must be nonzero" );
            }
            if( keycode > std::numeric_limits<std::uint8_t>::max() )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "XTEST keycode is out of range" );
            }
            return static_cast<std::uint8_t>( keycode );
        }

        [[nodiscard]]
        grab::Result<std::uint8_t>
        required_modifier_keycode( std::uint32_t    keycode,
                                   std::string_view modifier )
        {
            if( keycode == 0U )
            {
                return grab::fail( grab::ErrorCode::UnsupportedCharacter,
                                   "keymap does not provide the required " +
                                       std::string{ modifier } +
                                       " modifier" );
            }
            return xtest_keycode( keycode );
        }

        [[nodiscard]]
        grab::Result<void>
        press_modifier( grab::input::Seat&                seat,
                        std::uint8_t                      keycode,
                        std::array<std::uint8_t,
                                   maximumModifierCount>& modifiers,
                        std::size_t&                      modifier_count )
        {
            auto press_result = seat.key( keycode, true );
            if( !press_result.has_value() )
            {
                auto cleanup_result =
                    release_modifiers( seat, modifiers, modifier_count );
                static_cast<void>( cleanup_result );
                return error_from( press_result );
            }

            modifiers.at( modifier_count ) = keycode;
            ++modifier_count;
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        synthesize_keystroke( grab::input::Seat&     seat,
                              const grab::Keystroke& keystroke,
                              std::uint8_t           shift_keycode,
                              std::uint8_t           altgr_keycode )
        {
            std::array<std::uint8_t, maximumModifierCount> modifiers{};
            std::size_t                                    modifier_count = 0U;

            auto base_keycode = xtest_keycode( keystroke.keycode );
            if( !base_keycode.has_value() )
            {
                return std::unexpected( std::move( base_keycode.error() ) );
            }

            if( keystroke.shift )
            {
                auto press_result =
                    press_modifier( seat, shift_keycode, modifiers, modifier_count );
                if( !press_result.has_value() )
                {
                    return press_result;
                }
            }
            if( keystroke.altgr )
            {
                auto press_result =
                    press_modifier( seat, altgr_keycode, modifiers, modifier_count );
                if( !press_result.has_value() )
                {
                    return press_result;
                }
            }

            auto base_press = seat.key( *base_keycode, true );
            if( !base_press.has_value() )
            {
                auto cleanup_result =
                    release_modifiers( seat, modifiers, modifier_count );
                static_cast<void>( cleanup_result );
                return error_from( base_press );
            }

            auto base_release = seat.key( *base_keycode, false );
            if( !base_release.has_value() )
            {
                auto cleanup_result =
                    release_modifiers( seat, modifiers, modifier_count );
                static_cast<void>( cleanup_result );
                return error_from( base_release );
            }

            return release_modifiers( seat, modifiers, modifier_count );
        }

    }    // namespace

    Input::Input( std::unique_ptr<Impl> impl ) noexcept :
        impl_( std::move( impl ) )
    {
    }

    Input::~Input()                        = default;

    Input::Input( Input&& other ) noexcept = default;

    Input&
    Input::operator=( Input&& other ) noexcept = default;

    grab::Result<Input::Impl*>
    Input::require_impl() noexcept
    {
        if( impl_ == nullptr )
        {
            return grab::fail( grab::ErrorCode::InternalFault, "Input is not open" );
        }
        return impl_.get();
    }

    grab::Result<Input>
    Input::open( const char*      display,
                 std::string_view layout )
    {
        auto seat = grab::input::Seat::open( display );
        if( !seat.has_value() )
        {
            return std::unexpected( std::move( seat.error() ) );
        }

        std::optional<grab::Keymap> keymap;
        if( !layout.empty() )
        {
            auto explicit_keymap =
                grab::platform::x11::make_keymap_from_layout( layout );
            if( !explicit_keymap.has_value() )
            {
                return std::unexpected( std::move( explicit_keymap.error() ) );
            }
            keymap.emplace( std::move( *explicit_keymap ) );
        }

        return Input{ std::make_unique<Impl>( std::move( *seat ),
                                              std::move( keymap ),
                                              display == nullptr
                                                  ? std::string{}
                                                  : std::string{ display },
                                              layout.empty() ) };
    }

    grab::Result<grab::Keymap*>
    Input::Impl::ensure_keymap()
    {
        if( keymap_.has_value() && !server_keymap_ )
        {
            return &*keymap_;
        }

        auto connection = grab::platform::x11::XcbConnection::open( display_ );
        if( !connection.has_value() )
        {
            return std::unexpected( std::move( connection.error() ) );
        }

        auto keymap = grab::platform::x11::make_keymap_from_connection( *connection );
        if( !keymap.has_value() )
        {
            return std::unexpected( std::move( keymap.error() ) );
        }

        keymap_.emplace( std::move( *keymap ) );
        return &*keymap_;
    }

    grab::Result<grab::geometry::Point>
    Input::position()
    {
        auto state = require_impl();
        if( !state.has_value() )
        {
            return std::unexpected( std::move( state.error() ) );
        }
        return ( *state )->seat_.pointer_position();
    }

    grab::Result<void>
    Input::move( std::int16_t x,
                 std::int16_t y )
    {
        auto state = require_impl();
        if( !state.has_value() )
        {
            return std::unexpected( std::move( state.error() ) );
        }

        auto move_result = ( *state )->seat_.move_pointer_absolute( x, y );
        if( !move_result.has_value() )
        {
            return error_from( move_result );
        }
        return ( *state )->seat_.flush();
    }

    grab::Result<void>
    Input::click( std::uint8_t button )
    {
        auto state = require_impl();
        if( !state.has_value() )
        {
            return std::unexpected( std::move( state.error() ) );
        }

        auto press_result = ( *state )->seat_.button( button, true );
        if( !press_result.has_value() )
        {
            return error_from( press_result );
        }

        auto release_result = ( *state )->seat_.button( button, false );
        if( !release_result.has_value() )
        {
            return error_from( release_result );
        }

        return ( *state )->seat_.flush();
    }

    grab::Result<void>
    Input::press( std::uint8_t button )
    {
        auto state = require_impl();
        if( !state.has_value() )
        {
            return std::unexpected( std::move( state.error() ) );
        }

        auto press_result = ( *state )->seat_.button( button, true );
        if( !press_result.has_value() )
        {
            return error_from( press_result );
        }
        return ( *state )->seat_.flush();
    }

    grab::Result<void>
    Input::release( std::uint8_t button )
    {
        auto state = require_impl();
        if( !state.has_value() )
        {
            return std::unexpected( std::move( state.error() ) );
        }

        auto release_result = ( *state )->seat_.button( button, false );
        if( !release_result.has_value() )
        {
            return error_from( release_result );
        }
        return ( *state )->seat_.flush();
    }

    grab::Result<void>
    Input::scroll( std::int32_t dx,
                   std::int32_t dy )
    {
        auto state = require_impl();
        if( !state.has_value() )
        {
            return std::unexpected( std::move( state.error() ) );
        }

        // A wheel notch is a press and a release of the direction's button.
        const auto notches = [&]( std::int32_t count,
                                  std::uint8_t button ) -> grab::Result<void>
        {
            for( std::int32_t emitted = 0; emitted < count; ++emitted )
            {
                auto pressed = ( *state )->seat_.button( button, true );
                if( !pressed.has_value() )
                {
                    return error_from( pressed );
                }
                auto released = ( *state )->seat_.button( button, false );
                if( !released.has_value() )
                {
                    return error_from( released );
                }
            }
            return grab::Result<void>{};
        };

        // std::abs on the most negative value is undefined, so negate in a wider
        // type: a caller passing INT32_MIN gets a large scroll, not a trap.
        const auto magnitude = []( std::int32_t value ) -> std::int32_t
        {
            const std::int64_t widened  = value;
            const std::int64_t absolute = widened < 0 ? -widened : widened;
            return static_cast<std::int32_t>(
                std::min<std::int64_t>( absolute,
                                        std::numeric_limits<std::int32_t>::max() )
            );
        };

        if( dy != 0 )
        {
            const auto result =
                notches( magnitude( dy ),
                         button_code( dy > 0 ? grab::input::PointerButton::WheelDown
                                             : grab::input::PointerButton::WheelUp ) );
            if( !result.has_value() )
            {
                return result;
            }
        }
        if( dx != 0 )
        {
            const auto result =
                notches( magnitude( dx ),
                         button_code( dx > 0 ? grab::input::PointerButton::WheelRight
                                             : grab::input::PointerButton::WheelLeft ) );
            if( !result.has_value() )
            {
                return result;
            }
        }
        return ( *state )->seat_.flush();
    }

    grab::Result<void>
    Input::click_at( std::int16_t x,
                     std::int16_t y,
                     std::uint8_t button )
    {
        auto state = require_impl();
        if( !state.has_value() )
        {
            return std::unexpected( std::move( state.error() ) );
        }

        auto move_result = ( *state )->seat_.move_pointer_absolute( x, y );
        if( !move_result.has_value() )
        {
            return error_from( move_result );
        }

        auto flush_result = ( *state )->seat_.flush();
        if( !flush_result.has_value() )
        {
            return error_from( flush_result );
        }

        return click( button );
    }

    grab::Result<void>
    Input::drag( grab::input::Point              from,
                 grab::input::Point              to,
                 const grab::input::DragOptions& options )
    {
        auto state = require_impl();
        if( !state.has_value() )
        {
            return std::unexpected( std::move( state.error() ) );
        }
        return grab::drivers::desktop::x11::execute_drag( ( *state )->seat_,
                                                          from,
                                                          to,
                                                          options );
    }

    grab::Result<void>
    Input::type_text( std::string_view utf8 )
    {
        auto state = require_impl();
        if( !state.has_value() )
        {
            return std::unexpected( std::move( state.error() ) );
        }

        auto keymap = ( *state )->ensure_keymap();
        if( !keymap.has_value() )
        {
            return std::unexpected( std::move( keymap.error() ) );
        }

        auto keystrokes = ( *keymap )->text_to_keystrokes( utf8 );
        if( !keystrokes.has_value() )
        {
            return std::unexpected( std::move( keystrokes.error() ) );
        }

        const bool needs_shift =
            std::ranges::any_of( *keystrokes,
                                 []( const grab::Keystroke& keystroke )
                                 {
                                     return keystroke.shift;
                                 } );
        const bool needs_altgr =
            std::ranges::any_of( *keystrokes,
                                 []( const grab::Keystroke& keystroke )
                                 {
                                     return keystroke.altgr;
                                 } );

        std::uint8_t shift_keycode = 0U;
        if( needs_shift )
        {
            auto resolved =
                required_modifier_keycode( ( *keymap )->shift_keycode(), "Shift" );
            if( !resolved.has_value() )
            {
                return std::unexpected( std::move( resolved.error() ) );
            }
            shift_keycode = *resolved;
        }

        std::uint8_t altgr_keycode = 0U;
        if( needs_altgr )
        {
            auto resolved =
                required_modifier_keycode( ( *keymap )->altgr_keycode(), "AltGr" );
            if( !resolved.has_value() )
            {
                return std::unexpected( std::move( resolved.error() ) );
            }
            altgr_keycode = *resolved;
        }

        for( const grab::Keystroke& keystroke : *keystrokes )
        {
            auto key_result = synthesize_keystroke( ( *state )->seat_,
                                                    keystroke,
                                                    shift_keycode,
                                                    altgr_keycode );
            if( !key_result.has_value() )
            {
                return key_result;
            }
        }

        return ( *state )->seat_.flush();
    }

    namespace
    {

        // Shared by key_down and key_up: resolve a name to its keycode without
        // applying the layout's shift/altgr levels. Holding a key is about the
        // physical key, not about which character that key would produce.
        [[nodiscard]]
        grab::Result<std::uint8_t>
        modifier_keycode_for( grab::Keymap&    keymap,
                              std::string_view name )
        {
            const auto keystroke = keymap.keystroke_for_key( name );
            if( !keystroke.has_value() )
            {
                return grab::fail( grab::ErrorCode::UnsupportedCharacter,
                                   "named key is not available in keymap: " +
                                       std::string{ name } );
            }
            if( keystroke->keycode == 0U )
            {
                return grab::fail( grab::ErrorCode::UnsupportedCharacter,
                                   "named key has no keycode in this layout: " +
                                       std::string{ name } );
            }
            return static_cast<std::uint8_t>( keystroke->keycode );
        }

    }    // namespace

    grab::Result<void>
    Input::key_down( std::string_view name )
    {
        auto state = require_impl();
        if( !state.has_value() )
        {
            return std::unexpected( std::move( state.error() ) );
        }
        auto keymap = ( *state )->ensure_keymap();
        if( !keymap.has_value() )
        {
            return std::unexpected( std::move( keymap.error() ) );
        }
        auto keycode = modifier_keycode_for( **keymap, name );
        if( !keycode.has_value() )
        {
            return std::unexpected( std::move( keycode.error() ) );
        }
        auto pressed = ( *state )->seat_.key( *keycode, true );
        if( !pressed.has_value() )
        {
            return error_from( pressed );
        }
        return ( *state )->seat_.flush();
    }

    grab::Result<void>
    Input::key_up( std::string_view name )
    {
        auto state = require_impl();
        if( !state.has_value() )
        {
            return std::unexpected( std::move( state.error() ) );
        }
        auto keymap = ( *state )->ensure_keymap();
        if( !keymap.has_value() )
        {
            return std::unexpected( std::move( keymap.error() ) );
        }
        auto keycode = modifier_keycode_for( **keymap, name );
        if( !keycode.has_value() )
        {
            return std::unexpected( std::move( keycode.error() ) );
        }
        auto released = ( *state )->seat_.key( *keycode, false );
        if( !released.has_value() )
        {
            return error_from( released );
        }
        return ( *state )->seat_.flush();
    }

    grab::Result<void>
    Input::press_key( std::string_view name )
    {
        auto state = require_impl();
        if( !state.has_value() )
        {
            return std::unexpected( std::move( state.error() ) );
        }

        auto keymap = ( *state )->ensure_keymap();
        if( !keymap.has_value() )
        {
            return std::unexpected( std::move( keymap.error() ) );
        }

        const auto keystroke = ( *keymap )->keystroke_for_key( name );
        if( !keystroke.has_value() )
        {
            return grab::fail( grab::ErrorCode::UnsupportedCharacter,
                               "named key is not available in keymap: " +
                                   std::string{ name } );
        }

        std::uint8_t shift_keycode = 0U;
        if( keystroke->shift )
        {
            auto resolved =
                required_modifier_keycode( ( *keymap )->shift_keycode(), "Shift" );
            if( !resolved.has_value() )
            {
                return std::unexpected( std::move( resolved.error() ) );
            }
            shift_keycode = *resolved;
        }

        std::uint8_t altgr_keycode = 0U;
        if( keystroke->altgr )
        {
            auto resolved =
                required_modifier_keycode( ( *keymap )->altgr_keycode(), "AltGr" );
            if( !resolved.has_value() )
            {
                return std::unexpected( std::move( resolved.error() ) );
            }
            altgr_keycode = *resolved;
        }

        auto key_result = synthesize_keystroke( ( *state )->seat_,
                                                *keystroke,
                                                shift_keycode,
                                                altgr_keycode );
        if( !key_result.has_value() )
        {
            return key_result;
        }
        return ( *state )->seat_.flush();
    }

}    // namespace grab
