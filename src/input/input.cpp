#include "grab/input.hpp"
#include "grab/keymap.hpp"
#include "grab/result.hpp"
#include "input/gestures.hpp"
#include "input/locator.hpp"
#include "input/seat.hpp"
#include "platform/x11/xcb_connection.hpp"
#include "platform/x11/xkb_keymap.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace grab
{

    namespace
    {

        constexpr std::size_t maximumModifierCount  = 2U;
        constexpr double      minimumWindowFraction = 0.0;
        constexpr double      maximumWindowFraction = 1.0;

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

        [[nodiscard]]
        grab::Result<std::int16_t>
        coordinate_from_fraction( std::int32_t     origin,
                                  std::uint32_t    size,
                                  double           fraction,
                                  std::string_view axis )
        {
            if( !std::isfinite( fraction ) ||
                fraction <
                minimumWindowFraction ||
                fraction > maximumWindowFraction )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   std::string{ axis } +
                                       " fraction must be between 0 and 1" );
            }
            if( size == 0U )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   std::string{ axis } +
                                       " window extent must be nonzero" );
            }

            const auto   maximum_offset = static_cast<double>( size - 1U );
            const double absolute =
                static_cast<double>( origin ) + std::round( fraction * maximum_offset );
            if( absolute <
                static_cast<double>( std::numeric_limits<std::int16_t>::min() ) ||
                absolute >
                static_cast<double>( std::numeric_limits<std::int16_t>::max() ) )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   std::string{ axis } +
                                       " coordinate is outside int16 range" );
            }

            return static_cast<std::int16_t>( absolute );
        }

    }    // namespace

    Input::Input( grab::input::Seat           seat,
                  std::optional<grab::Keymap> keymap,
                  grab::input::WindowLocator  locator,
                  std::string                 display,
                  bool                        server_keymap ) noexcept :
        seat_( std::move( seat ) ),
        keymap_( std::move( keymap ) ),
        locator_( std::move( locator ) ),
        display_( std::move( display ) ),
        server_keymap_( server_keymap )
    {
    }

    Input::~Input() = default;

    Input::Input( Input&& other ) noexcept :
        seat_( std::move( other.seat_ ) ),
        keymap_( std::move( other.keymap_ ) ),
        locator_( std::move( other.locator_ ) ),
        display_( std::move( other.display_ ) ),
        server_keymap_( other.server_keymap_ )
    {
    }

    Input&
    Input::operator=( Input&& other ) noexcept
    {
        if( this != &other )
        {
            seat_          = std::move( other.seat_ );
            keymap_        = std::move( other.keymap_ );
            locator_       = std::move( other.locator_ );
            display_       = std::move( other.display_ );
            server_keymap_ = other.server_keymap_;
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

        auto locator = grab::input::WindowLocator::open( display );
        if( !locator.has_value() )
        {
            return std::unexpected( std::move( locator.error() ) );
        }

        return Input{
            std::move( *seat ),
            std::move( keymap ),
            std::move( *locator ),
            display == nullptr ? std::string{} : std::string{ display },
            layout.empty(),
        };
    }

    grab::Result<grab::Keymap*>
    Input::ensure_keymap()
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
    Input::drag( grab::input::Point              from,
                 grab::input::Point              to,
                 const grab::input::DragOptions& options )
    {
        return grab::input::linear_drag( seat_, from, to, options );
    }

    grab::Result<void>
    Input::type_text( std::string_view utf8 )
    {
        auto keymap = ensure_keymap();
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
            auto key_result =
                synthesize_keystroke( seat_, keystroke, shift_keycode, altgr_keycode );
            if( !key_result.has_value() )
            {
                return key_result;
            }
        }

        return seat_.flush();
    }

    grab::Result<void>
    Input::press_key( std::string_view name )
    {
        auto keymap = ensure_keymap();
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

        auto key_result =
            synthesize_keystroke( seat_, *keystroke, shift_keycode, altgr_keycode );
        if( !key_result.has_value() )
        {
            return key_result;
        }
        return seat_.flush();
    }

    grab::Result<void>
    Input::activate( const grab::input::LocatedWindow& win )
    {
        return locator_.activate( win );
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
        if( win.trust == grab::input::GeometryTrust::Unavailable )
        {
            return grab::fail( grab::ErrorCode::GeometryUntrusted,
                               "window geometry is unavailable" );
        }

        auto x = coordinate_from_fraction( win.bounds.x, win.bounds.width, frac_x, "x" );
        if( !x.has_value() )
        {
            return std::unexpected( std::move( x.error() ) );
        }

        auto y =
            coordinate_from_fraction( win.bounds.y, win.bounds.height, frac_y, "y" );
        if( !y.has_value() )
        {
            return std::unexpected( std::move( y.error() ) );
        }

        return click_at( *x, *y, button );
    }

    grab::Result<void>
    Input::drag_curve_in_window( const grab::input::LocatedWindow& win,
                                 double                            source_x,
                                 double                            source_y,
                                 double                            destination_x,
                                 double                            destination_y,
                                 const grab::input::DragOptions&   options )
    {
        if( win.trust == grab::input::GeometryTrust::Unavailable )
        {
            return grab::fail( grab::ErrorCode::GeometryUntrusted,
                               "window geometry is unavailable" );
        }

        auto from_x = coordinate_from_fraction( win.bounds.x,
                                                win.bounds.width,
                                                source_x,
                                                "source x" );
        if( !from_x.has_value() )
        {
            return std::unexpected( std::move( from_x.error() ) );
        }
        auto from_y = coordinate_from_fraction( win.bounds.y,
                                                win.bounds.height,
                                                source_y,
                                                "source y" );
        if( !from_y.has_value() )
        {
            return std::unexpected( std::move( from_y.error() ) );
        }
        auto to_x = coordinate_from_fraction( win.bounds.x,
                                              win.bounds.width,
                                              destination_x,
                                              "destination x" );
        if( !to_x.has_value() )
        {
            return std::unexpected( std::move( to_x.error() ) );
        }
        auto to_y = coordinate_from_fraction( win.bounds.y,
                                              win.bounds.height,
                                              destination_y,
                                              "destination y" );
        if( !to_y.has_value() )
        {
            return std::unexpected( std::move( to_y.error() ) );
        }

        return grab::input::curve_drag( seat_,
                                        grab::input::Point{ .x = *from_x, .y = *from_y },
                                        grab::input::Point{ .x = *to_x, .y = *to_y },
                                        options );
    }

}    // namespace grab
