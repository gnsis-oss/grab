#include "grab/image.hpp"
#include "grab/result.hpp"
#include "grab/screen.hpp"
#include "screen/enumerate.hpp"
#include "screen/x11_capture.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <xcb/xcb.h>
#include <xcb/xproto.h>

namespace grab
{
    namespace
    {

        constexpr int              kXcbOk                      = 0;
        constexpr std::uint8_t     kX11SuccessResponse         = 1U;
        constexpr std::uint8_t     kFormat32Bits               = 32U;
        constexpr std::uint32_t    kPropertyOffsetZero         = 0U;
        constexpr std::uint32_t    kSingleWindowPropertyLength = 1U;
        constexpr unsigned char    kAsciiUpperA                = 'A';
        constexpr unsigned char    kAsciiUpperZ                = 'Z';
        constexpr unsigned char    kAsciiCaseOffset            = 'a' - 'A';
        constexpr std::string_view kNetActiveWindowAtom        = "_NET_ACTIVE_WINDOW";

        template<typename T>
        using XcbOwned = std::unique_ptr<T, decltype( &std::free )>;

        using XcbConnection =
            std::unique_ptr<xcb_connection_t, decltype( &xcb_disconnect )>;

        struct ActiveDisplay
        {
                XcbConnection connection;
                xcb_window_t  root = XCB_WINDOW_NONE;
        };

        template<typename T>
        [[nodiscard]]
        XcbOwned<T>
        take_xcb_owned( T* pointer ) noexcept
        {
            return XcbOwned<T>{ pointer, &std::free };
        }

        [[nodiscard]]
        char
        ascii_lower( char value ) noexcept
        {
            const auto code = static_cast<unsigned char>( value );
            if( code >= kAsciiUpperA && code <= kAsciiUpperZ )
            {
                return static_cast<char>( code + kAsciiCaseOffset );
            }
            return value;
        }

        [[nodiscard]]
        std::string
        ascii_lower_copy( std::string_view text )
        {
            std::string result;
            result.reserve( text.size() );
            std::ranges::transform( text, std::back_inserter( result ), ascii_lower );
            return result;
        }

        [[nodiscard]]
        std::vector<std::string>
        normalized_candidates( const std::vector<std::string>& candidates )
        {
            std::vector<std::string> result;
            result.reserve( candidates.size() );
            for( const std::string& candidate : candidates )
            {
                if( !candidate.empty() )
                {
                    result.push_back( ascii_lower_copy( candidate ) );
                }
            }
            return result;
        }

        [[nodiscard]]
        bool
        wm_class_matches( std::string_view                wm_class,
                          const std::vector<std::string>& candidates )
        {
            const std::string lowered_class = ascii_lower_copy( wm_class );
            return std::ranges::any_of( candidates,
                                        [&lowered_class]( const std::string& candidate )
                                        {
                                            return lowered_class.contains( candidate );
                                        } );
        }

        [[nodiscard]]
        grab::Result<ActiveDisplay>
        connect_active_display( const char* display )
        {
            int           screen_index = 0;
            XcbConnection connection{
                xcb_connect( display, &screen_index ),
                &xcb_disconnect
            };
            if( connection ==
                nullptr ||
                xcb_connection_has_error( connection.get() ) != kXcbOk )
            {
                return grab::fail( grab::ErrorCode::device_inaccessible,
                                   "XCB display connection failed" );
            }

            const xcb_setup_t* const setup = xcb_get_setup( connection.get() );
            if( setup == nullptr || setup->status != kX11SuccessResponse )
            {
                return grab::fail( grab::ErrorCode::device_inaccessible,
                                   "XCB setup is unavailable" );
            }

            xcb_screen_iterator_t iterator = xcb_setup_roots_iterator( setup );
            for( int current_screen = 0;
                 current_screen < screen_index && iterator.rem > 0;
                 ++current_screen )
            {
                xcb_screen_next( &iterator );
            }

            if( iterator.data == nullptr )
            {
                return grab::fail( grab::ErrorCode::device_inaccessible,
                                   "XCB default screen is unavailable" );
            }

            return ActiveDisplay{
                .connection = std::move( connection ),
                .root       = iterator.data->root,
            };
        }

        [[nodiscard]]
        grab::Result<xcb_atom_t>
        intern_active_window_atom( xcb_connection_t* connection )
        {
            xcb_generic_error_t* raw_error = nullptr;
            const auto           reply     = take_xcb_owned(
                xcb_intern_atom_reply( connection,
                                       xcb_intern_atom( connection,
                                                        1U,
                                                        static_cast<std::uint16_t>(
                                                            kNetActiveWindowAtom.size()
                                                        ),
                                                        kNetActiveWindowAtom.data() ),
                                       &raw_error )
            );
            const auto error = take_xcb_owned( raw_error );
            if( error != nullptr || reply == nullptr )
            {
                return grab::fail( grab::ErrorCode::protocol_error,
                                   "XCB atom lookup failed for _NET_ACTIVE_WINDOW" );
            }
            if( reply->atom == XCB_ATOM_NONE )
            {
                return grab::fail( grab::ErrorCode::window_not_found,
                                   "EWMH active window atom is unavailable" );
            }
            return reply->atom;
        }

        [[nodiscard]]
        grab::Result<xcb_window_t>
        read_active_window_property( xcb_connection_t* connection,
                                     xcb_window_t      root,
                                     xcb_atom_t        active_window_atom )
        {
            xcb_generic_error_t* raw_error = nullptr;
            const auto           reply     = take_xcb_owned(
                xcb_get_property_reply( connection,
                                        xcb_get_property( connection,
                                                          0U,
                                                          root,
                                                          active_window_atom,
                                                          XCB_ATOM_WINDOW,
                                                          kPropertyOffsetZero,
                                                          kSingleWindowPropertyLength ),
                                        &raw_error )
            );
            const auto error = take_xcb_owned( raw_error );
            if( error != nullptr )
            {
                return grab::fail( grab::ErrorCode::protocol_error,
                                   "XCB active window property read failed" );
            }
            if( reply ==
                nullptr ||
                reply->type ==
                XCB_ATOM_NONE ||
                xcb_get_property_value_length( reply.get() ) == 0 )
            {
                return grab::fail( grab::ErrorCode::window_not_found,
                                   "EWMH active window is unset" );
            }
            if( reply->format != kFormat32Bits )
            {
                return grab::fail( grab::ErrorCode::protocol_error,
                                   "EWMH active window property has an invalid format" );
            }

            const int value_length = xcb_get_property_value_length( reply.get() );
            if( std::cmp_less( value_length, sizeof( xcb_window_t ) ) )
            {
                return grab::fail( grab::ErrorCode::protocol_error,
                                   "EWMH active window property is truncated" );
            }

            const auto* const bytes =
                static_cast<const std::byte*>( xcb_get_property_value( reply.get() ) );
            std::array<std::byte, sizeof( xcb_window_t )> raw_window{};
            const auto                                    window_bytes =
                std::span<const std::byte>{ bytes, raw_window.size() };
            std::ranges::copy( window_bytes, raw_window.begin() );

            const auto window = std::bit_cast<xcb_window_t>( raw_window );
            if( window == XCB_WINDOW_NONE )
            {
                return grab::fail( grab::ErrorCode::window_not_found,
                                   "EWMH active window is unset" );
            }
            return window;
        }

        [[nodiscard]]
        grab::Result<xcb_window_t>
        read_active_window_id( const char* display )
        {
            auto active_display = connect_active_display( display );
            if( !active_display.has_value() )
            {
                return std::unexpected( std::move( active_display.error() ) );
            }

            auto atom = intern_active_window_atom( active_display->connection.get() );
            if( !atom.has_value() )
            {
                return std::unexpected( std::move( atom.error() ) );
            }

            return read_active_window_property( active_display->connection.get(),
                                                active_display->root,
                                                *atom );
        }

    }    // namespace

    struct Screen::Impl
    {
            screen::X11Capturer        capturer;
            std::optional<std::string> display;

            Impl( screen::X11Capturer capturer_value,
                  const char*         display_value ) :
                capturer( std::move( capturer_value ) ),
                display( display_value == nullptr ? std::optional<std::string>{}
                                                  : std::optional<std::string>{
                                                        std::string{ display_value }
                                                    } )
            {
            }

            [[nodiscard]]
            const char*
            display_name() const noexcept
            {
                if( display.has_value() )
                {
                    return display->c_str();
                }
                return nullptr;
            }
    };

    Screen::Screen( std::unique_ptr<Impl> impl ) noexcept :
        impl_( std::move( impl ) )
    {
    }

    Screen::~Screen()                         = default;

    Screen::Screen( Screen&& other ) noexcept = default;

    Screen&
    Screen::operator=( Screen&& other ) noexcept = default;

    grab::Result<Screen>
    Screen::open( const char* display )
    {
        auto capturer = screen::X11Capturer::open( display );
        if( !capturer.has_value() )
        {
            return std::unexpected( std::move( capturer.error() ) );
        }

        return Screen{ std::make_unique<Impl>( std::move( *capturer ), display ) };
    }

    grab::Result<Image>
    Screen::window( std::uint32_t id )
    {
        if( impl_ == nullptr )
        {
            return grab::fail( grab::ErrorCode::internal_fault, "Screen is not open" );
        }
        return impl_->capturer.capture_window( id );
    }

    grab::Result<Image>
    Screen::window_by_class( const std::vector<std::string>& wm_class_candidates )
    {
        if( impl_ == nullptr )
        {
            return grab::fail( grab::ErrorCode::internal_fault, "Screen is not open" );
        }

        const std::vector<std::string> candidates =
            normalized_candidates( wm_class_candidates );
        if( candidates.empty() )
        {
            return grab::fail( grab::ErrorCode::window_not_found,
                               "no WM_CLASS candidates were provided" );
        }

        auto windows = screen::list_windows( impl_->display_name() );
        if( !windows.has_value() )
        {
            return std::unexpected( std::move( windows.error() ) );
        }

        for( const screen::WindowInfo& info : *windows )
        {
            if( wm_class_matches( info.wm_class, candidates ) )
            {
                return impl_->capturer.capture_window( info.id );
            }
        }

        return grab::fail( grab::ErrorCode::window_not_found,
                           "no window matched the requested WM_CLASS" );
    }

    grab::Result<Image>
    Screen::display()
    {
        if( impl_ == nullptr )
        {
            return grab::fail( grab::ErrorCode::internal_fault, "Screen is not open" );
        }
        return impl_->capturer.capture_display();
    }

    grab::Result<Image>
    Screen::region( std::int16_t  x,
                    std::int16_t  y,
                    std::uint16_t width,
                    std::uint16_t height )
    {
        if( impl_ == nullptr )
        {
            return grab::fail( grab::ErrorCode::internal_fault, "Screen is not open" );
        }
        return impl_->capturer.capture_region( x, y, width, height );
    }

    grab::Result<Image>
    Screen::active_window()
    {
        if( impl_ == nullptr )
        {
            return grab::fail( grab::ErrorCode::internal_fault, "Screen is not open" );
        }

        auto active_window_id = read_active_window_id( impl_->display_name() );
        if( !active_window_id.has_value() )
        {
            return std::unexpected( std::move( active_window_id.error() ) );
        }

        return impl_->capturer.capture_window( *active_window_id );
    }

}    // namespace grab
