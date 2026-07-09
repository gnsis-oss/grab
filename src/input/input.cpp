#include "grab/input.hpp"
#include "grab/result.hpp"
#include "input/gestures.hpp"
#include "input/keymap.hpp"
#include "input/locator.hpp"
#include "input/seat.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace grab
{

    namespace
    {

        constexpr std::size_t kMaximumModifierCount = 2U;

        [[nodiscard]]
        grab::Result<void>
        error_from( grab::Result<void>& result )
        {
            return std::unexpected( std::move( result.error() ) );
        }

        [[nodiscard]]
        grab::Result<void>
        release_modifiers( grab::input::Seat&                       seat,
                           const std::array<std::uint8_t,
                                            kMaximumModifierCount>& modifiers,
                           std::size_t                              modifier_count )
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
        grab::Result<void>
        press_modifier( grab::input::Seat&                 seat,
                        std::uint8_t                       keycode,
                        std::array<std::uint8_t,
                                   kMaximumModifierCount>& modifiers,
                        std::size_t&                       modifier_count )
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
        synthesize_keystroke( grab::input::Seat&            seat,
                              const grab::input::Keystroke& keystroke,
                              std::uint8_t                  shift_keycode,
                              std::uint8_t                  level3_keycode )
        {
            std::array<std::uint8_t, kMaximumModifierCount> modifiers{};
            std::size_t                                     modifier_count = 0U;

            if( keystroke.shift )
            {
                auto press_result =
                    press_modifier( seat, shift_keycode, modifiers, modifier_count );
                if( !press_result.has_value() )
                {
                    return press_result;
                }
            }
            if( keystroke.level3 )
            {
                auto press_result =
                    press_modifier( seat, level3_keycode, modifiers, modifier_count );
                if( !press_result.has_value() )
                {
                    return press_result;
                }
            }

            auto base_press = seat.key( keystroke.keycode, true );
            if( !base_press.has_value() )
            {
                auto cleanup_result =
                    release_modifiers( seat, modifiers, modifier_count );
                static_cast<void>( cleanup_result );
                return error_from( base_press );
            }

            auto base_release = seat.key( keystroke.keycode, false );
            if( !base_release.has_value() )
            {
                auto cleanup_result =
                    release_modifiers( seat, modifiers, modifier_count );
                static_cast<void>( cleanup_result );
                return error_from( base_release );
            }

            return release_modifiers( seat, modifiers, modifier_count );
        }

        [[nodiscard]]
        grab::Result<std::int16_t>
        coordinate_from_fraction( std::int16_t     origin,
                                  std::uint16_t    size,
                                  double           fraction,
                                  std::string_view axis )
        {
            if( !std::isfinite( fraction ) )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   std::string{ axis } + " fraction is not finite" );
            }

            const double absolute = static_cast<double>( origin ) +
                                    std::round( fraction * static_cast<double>( size ) );
            if( absolute <
                static_cast<double>( std::numeric_limits<std::int16_t>::min() ) ||
                absolute >
                static_cast<double>( std::numeric_limits<std::int16_t>::max() ) )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   std::string{ axis } +
                                       " coordinate is outside int16 range" );
            }

            return static_cast<std::int16_t>( absolute );
        }

    }    // namespace

    Input::Input( grab::input::Seat          seat,
                  grab::input::Keymap        keymap,
                  grab::input::WindowLocator locator ) noexcept :
        seat_( std::move( seat ) ),
        keymap_( std::move( keymap ) ),
        locator_( std::move( locator ) )
    {
    }

    Input::~Input() = default;

    Input::Input( Input&& other ) noexcept :
        seat_( std::move( other.seat_ ) ),
        keymap_( std::move( other.keymap_ ) ),
        locator_( std::move( other.locator_ ) )
    {
    }

    Input&
    Input::operator=( Input&& other ) noexcept
    {
        if( this != &other )
        {
            seat_    = std::move( other.seat_ );
            keymap_  = std::move( other.keymap_ );
            locator_ = std::move( other.locator_ );
        }
        return *this;
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

        auto keymap = grab::input::Keymap::open_layout( layout );
        if( !keymap.has_value() )
        {
            return std::unexpected( std::move( keymap.error() ) );
        }

        auto locator = grab::input::WindowLocator::open( display );
        if( !locator.has_value() )
        {
            return std::unexpected( std::move( locator.error() ) );
        }

        return Input{ std::move( *seat ), std::move( *keymap ), std::move( *locator ) };
    }

    grab::Result<void>
    Input::move( std::int16_t x,
                 std::int16_t y )
    {
        auto move_result = seat_.move_pointer_absolute( x, y );
        if( !move_result.has_value() )
        {
            return error_from( move_result );
        }
        return seat_.flush();
    }

    grab::Result<void>
    Input::click( std::uint8_t button )
    {
        auto press_result = seat_.button( button, true );
        if( !press_result.has_value() )
        {
            return error_from( press_result );
        }

        auto release_result = seat_.button( button, false );
        if( !release_result.has_value() )
        {
            return error_from( release_result );
        }

        return seat_.flush();
    }

    grab::Result<void>
    Input::click_at( std::int16_t x,
                     std::int16_t y,
                     std::uint8_t button )
    {
        auto move_result = seat_.move_pointer_absolute( x, y );
        if( !move_result.has_value() )
        {
            return error_from( move_result );
        }

        auto flush_result = seat_.flush();
        if( !flush_result.has_value() )
        {
            return error_from( flush_result );
        }

        return click( button );
    }

    grab::Result<void>
    Input::drag( grab::input::Point from,
                 grab::input::Point to )
    {
        return grab::input::qt_drag( seat_, from, to );
    }

    grab::Result<void>
    Input::type_text( std::string_view utf8 )
    {
        auto keystrokes = keymap_.text_to_keystrokes( utf8 );
        if( !keystrokes.has_value() )
        {
            return std::unexpected( std::move( keystrokes.error() ) );
        }

        const std::uint8_t shift_keycode  = keymap_.shift_keycode();
        const std::uint8_t level3_keycode = keymap_.level3_keycode();
        for( const grab::input::Keystroke& keystroke : *keystrokes )
        {
            auto key_result =
                synthesize_keystroke( seat_, keystroke, shift_keycode, level3_keycode );
            if( !key_result.has_value() )
            {
                return key_result;
            }
        }

        return seat_.flush();
    }

    grab::Result<grab::input::LocatedWindow>
    Input::locate( const std::vector<std::string>& wm_class_candidates,
                   std::string_view                title )
    {
        return locator_.locate( wm_class_candidates, title );
    }

    grab::Result<void>
    Input::click_in_window( const grab::input::LocatedWindow& win,
                            double                            frac_x,
                            double                            frac_y,
                            std::uint8_t                      button )
    {
        if( win.trust == grab::input::GeometryTrust::unavailable )
        {
            return grab::fail( grab::ErrorCode::geometry_untrusted,
                               "window geometry is unavailable" );
        }

        auto x = coordinate_from_fraction( win.x, win.width, frac_x, "x" );
        if( !x.has_value() )
        {
            return std::unexpected( std::move( x.error() ) );
        }

        auto y = coordinate_from_fraction( win.y, win.height, frac_y, "y" );
        if( !y.has_value() )
        {
            return std::unexpected( std::move( y.error() ) );
        }

        return click_at( *x, *y, button );
    }

}    // namespace grab
