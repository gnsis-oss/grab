#include "grab/result.hpp"
#include "grab/window.hpp"
#include "platform/x11/xcb_atom.hpp"
#include "platform/x11/xcb_connection.hpp"
#include "platform/x11/xcb_reply.hpp"
#include "platform/x11/xcb_window.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <xcb/xcb.h>
#include <xcb/xproto.h>

namespace grab::platform::x11
{

    namespace
    {

        constexpr std::uint8_t  string_terminator      = 0U;
        constexpr std::size_t   terminator_byte_count  = 1U;
        constexpr char          ascii_upper_a          = 'A';
        constexpr char          ascii_upper_z          = 'Z';
        constexpr char          ascii_lower_a          = 'a';
        constexpr std::uint8_t  keep_property          = 0U;
        constexpr std::uint8_t  expected_string_format = 8U;
        constexpr std::uint8_t  expected_window_format = 32U;
        constexpr std::uint32_t property_offset_start  = 0U;
        constexpr std::uint32_t read_entire_property =
            std::numeric_limits<std::uint32_t>::max();
        constexpr std::int16_t     origin_coordinate     = 0;
        constexpr xcb_atom_t       missing_atom          = XCB_ATOM_NONE;
        constexpr std::string_view client_list_atom_name = "_NET_CLIENT_LIST";
        constexpr std::string_view wm_class_atom_name    = "WM_CLASS";

        [[nodiscard]]
        char
        ascii_lower( char value ) noexcept
        {
            if( value >= ascii_upper_a && value <= ascii_upper_z )
            {
                return static_cast<char>( value - ascii_upper_a + ascii_lower_a );
            }
            return value;
        }

        [[nodiscard]]
        bool
        equal_case_insensitive( std::string_view left,
                                std::string_view right )
        {
            return std::ranges::equal( left,
                                       right,
                                       []( char left_char, char right_char ) noexcept
                                       {
                                           return ascii_lower( left_char ) ==
                                                  ascii_lower( right_char );
                                       } );
        }

        [[nodiscard]]
        bool
        contains_case_insensitive( std::string_view haystack,
                                   std::string_view needle )
        {
            if( needle.empty() )
            {
                return true;
            }
            if( haystack.size() < needle.size() )
            {
                return false;
            }

            const std::size_t last_offset = haystack.size() - needle.size();
            for( std::size_t offset = 0U; offset <= last_offset; ++offset )
            {
                if( equal_case_insensitive( haystack.substr( offset, needle.size() ),
                                            needle ) )
                {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]]
        std::size_t
        find_terminator( std::span<const std::uint8_t> raw ) noexcept
        {
            std::size_t index = 0U;
            for( const std::uint8_t byte : raw )
            {
                if( byte == string_terminator )
                {
                    return index;
                }
                ++index;
            }
            return raw.size();
        }

        [[nodiscard]]
        std::string
        bytes_to_string( std::span<const std::uint8_t> raw )
        {
            std::string value;
            value.reserve( raw.size() );
            for( const std::uint8_t byte : raw )
            {
                value.push_back( static_cast<char>( byte ) );
            }
            return value;
        }

        [[nodiscard]]
        grab::Result<XcbReply<xcb_get_property_reply_t>>
        read_window_property( const XcbConnection& conn,
                              xcb_window_t         window,
                              xcb_atom_t           property,
                              xcb_atom_t           type,
                              std::string_view     operation )
        {
            const xcb_get_property_cookie_t cookie =
                xcb_get_property( conn.get(),
                                  keep_property,
                                  window,
                                  property,
                                  type,
                                  property_offset_start,
                                  read_entire_property );
            xcb_generic_error_t* error_raw = nullptr;
            auto                 reply     = make_xcb_reply(
                xcb_get_property_reply( conn.get(), cookie, &error_raw )
            );
            auto error = make_xcb_reply( error_raw );

            if( error != nullptr || reply == nullptr )
            {
                return grab::fail( grab::ErrorCode::ProviderFailed,
                                   std::string{ operation } + " failed" );
            }
            return reply;
        }

        [[nodiscard]]
        grab::Result<std::size_t>
        property_value_byte_count( const xcb_get_property_reply_t& reply )
        {
            const int length = xcb_get_property_value_length( &reply );
            if( length < 0 )
            {
                return grab::fail( grab::ErrorCode::ProviderFailed,
                                   "xcb_get_property returned an invalid data length" );
            }
            return static_cast<std::size_t>( length );
        }

        [[nodiscard]]
        grab::Result<std::vector<xcb_window_t>>
        copy_client_list( const xcb_get_property_reply_t& reply )
        {
            auto byte_count = property_value_byte_count( reply );
            if( !byte_count.has_value() )
            {
                return grab::fail( byte_count.error().code, byte_count.error().message );
            }
            if( *byte_count % sizeof( xcb_window_t ) != 0U )
            {
                return grab::fail( grab::ErrorCode::ProviderFailed,
                                   "_NET_CLIENT_LIST has an invalid byte length" );
            }

            const std::size_t window_count = *byte_count / sizeof( xcb_window_t );
            if( window_count == 0U )
            {
                return std::vector<xcb_window_t>{};
            }

            const auto* data =
                static_cast<const xcb_window_t*>( xcb_get_property_value( &reply ) );
            if( data == nullptr )
            {
                return grab::fail( grab::ErrorCode::ProviderFailed,
                                   "_NET_CLIENT_LIST has no data" );
            }
            const std::span<const xcb_window_t> windows{ data, window_count };
            return std::vector<xcb_window_t>{ windows.begin(), windows.end() };
        }

        [[nodiscard]]
        grab::Result<std::vector<xcb_window_t>>
        get_client_list( const XcbConnection& conn,
                         xcb_atom_t           client_list_atom )
        {
            if( client_list_atom == missing_atom )
            {
                return std::vector<xcb_window_t>{};
            }

            auto reply = read_window_property( conn,
                                               conn.root(),
                                               client_list_atom,
                                               XCB_ATOM_WINDOW,
                                               "xcb_get_property _NET_CLIENT_LIST" );
            if( !reply.has_value() )
            {
                return grab::fail( reply.error().code, reply.error().message );
            }
            if( ( *reply )->type == missing_atom )
            {
                return std::vector<xcb_window_t>{};
            }
            if( ( *reply )->type !=
                XCB_ATOM_WINDOW ||
                ( *reply )->format != expected_window_format )
            {
                return grab::fail( grab::ErrorCode::ProviderFailed,
                                   "_NET_CLIENT_LIST has an unexpected type" );
            }
            return copy_client_list( **reply );
        }

        [[nodiscard]]
        grab::Result<WmClass>
        read_wm_class( const XcbConnection& conn,
                       xcb_window_t         window,
                       xcb_atom_t           wm_class_atom )
        {
            if( wm_class_atom == missing_atom )
            {
                return WmClass{};
            }

            auto reply = read_window_property( conn,
                                               window,
                                               wm_class_atom,
                                               XCB_ATOM_STRING,
                                               "xcb_get_property WM_CLASS" );
            if( !reply.has_value() )
            {
                return grab::fail( reply.error().code, reply.error().message );
            }
            if( ( *reply )->type !=
                XCB_ATOM_STRING ||
                ( *reply )->format != expected_string_format )
            {
                return WmClass{};
            }

            auto byte_count = property_value_byte_count( **reply );
            if( !byte_count.has_value() )
            {
                return grab::fail( byte_count.error().code, byte_count.error().message );
            }
            if( *byte_count == 0U )
            {
                return WmClass{};
            }

            const auto* data = static_cast<const std::uint8_t*>(
                xcb_get_property_value( reply->get() )
            );
            if( data == nullptr )
            {
                return grab::fail( grab::ErrorCode::ProviderFailed,
                                   "WM_CLASS has no data" );
            }
            return parse_wm_class( std::span<const std::uint8_t>{ data, *byte_count } );
        }

        [[nodiscard]]
        grab::Result<bool>
        is_viewable( const XcbConnection& conn,
                     xcb_window_t         window )
        {
            const xcb_get_window_attributes_cookie_t cookie =
                xcb_get_window_attributes( conn.get(), window );
            xcb_generic_error_t* error_raw = nullptr;
            auto                 reply     = make_xcb_reply(
                xcb_get_window_attributes_reply( conn.get(), cookie, &error_raw )
            );
            auto error = make_xcb_reply( error_raw );

            if( error != nullptr || reply == nullptr )
            {
                return grab::fail( grab::ErrorCode::ProviderFailed,
                                   "xcb_get_window_attributes failed" );
            }
            return reply->map_state == XCB_MAP_STATE_VIEWABLE;
        }

        [[nodiscard]]
        grab::Result<xcb_get_geometry_reply_t>
        read_geometry( const XcbConnection& conn,
                       xcb_window_t         window )
        {
            const xcb_get_geometry_cookie_t cookie =
                xcb_get_geometry( conn.get(), window );
            xcb_generic_error_t* error_raw = nullptr;
            auto                 reply     = make_xcb_reply(
                xcb_get_geometry_reply( conn.get(), cookie, &error_raw )
            );
            auto error = make_xcb_reply( error_raw );

            if( error != nullptr || reply == nullptr )
            {
                return grab::fail( grab::ErrorCode::ProviderFailed,
                                   "xcb_get_geometry failed for window" );
            }
            return *reply;
        }

        [[nodiscard]]
        grab::Result<xcb_translate_coordinates_reply_t>
        translate_origin( const XcbConnection& conn,
                          xcb_window_t         window )
        {
            const xcb_translate_coordinates_cookie_t cookie =
                xcb_translate_coordinates( conn.get(),
                                           window,
                                           conn.root(),
                                           origin_coordinate,
                                           origin_coordinate );
            xcb_generic_error_t* error_raw = nullptr;
            auto                 reply     = make_xcb_reply(
                xcb_translate_coordinates_reply( conn.get(), cookie, &error_raw )
            );
            auto error = make_xcb_reply( error_raw );

            if( error != nullptr || reply == nullptr )
            {
                return grab::fail( grab::ErrorCode::ProviderFailed,
                                   "xcb_translate_coordinates failed for window" );
            }
            return *reply;
        }

    }    // namespace

    WmClass
    parse_wm_class( std::span<const std::uint8_t> raw )
    {
        const std::size_t instance_end = find_terminator( raw );
        WmClass           parsed{
            .instance     = bytes_to_string( raw.first( instance_end ) ),
            .window_class = {},
        };

        if( instance_end == raw.size() )
        {
            return parsed;
        }

        const std::span<const std::uint8_t> class_bytes =
            raw.subspan( instance_end + terminator_byte_count );
        const std::size_t class_end = find_terminator( class_bytes );
        parsed.window_class         = bytes_to_string( class_bytes.first( class_end ) );
        return parsed;
    }

    bool
    class_matches( const WmClass&   wc,
                   std::string_view app )
    {
        return contains_case_insensitive( wc.instance, app ) ||
               contains_case_insensitive( wc.window_class, app );
    }

    grab::Result<WindowRef>
    find_window( const XcbConnection& conn,
                 const WindowMatch&   match )
    {
        auto client_list_atom =
            intern_atom( conn, client_list_atom_name, XcbAtomMode::OnlyIfExists );
        if( !client_list_atom.has_value() )
        {
            return grab::fail( client_list_atom.error().code,
                               client_list_atom.error().message );
        }
        auto wm_class_atom =
            intern_atom( conn, wm_class_atom_name, XcbAtomMode::OnlyIfExists );
        if( !wm_class_atom.has_value() )
        {
            return grab::fail( wm_class_atom.error().code,
                               wm_class_atom.error().message );
        }

        auto windows = get_client_list( conn, *client_list_atom );
        if( !windows.has_value() )
        {
            return grab::fail( windows.error().code, windows.error().message );
        }

        WindowRef last_match;
        for( const xcb_window_t window : *windows )
        {
            auto viewable = is_viewable( conn, window );
            if( !viewable.has_value() )
            {
                return grab::fail( viewable.error().code, viewable.error().message );
            }
            if( !*viewable )
            {
                continue;
            }

            auto wm_class = read_wm_class( conn, window, *wm_class_atom );
            if( !wm_class.has_value() )
            {
                return grab::fail( wm_class.error().code, wm_class.error().message );
            }
            if( class_matches( *wm_class, match.app ) )
            {
                last_match = WindowRef{
                    .id    = window,
                    .valid = true,
                };
            }
        }

        if( !last_match.valid )
        {
            return grab::fail( grab::ErrorCode::WindowNotFound,
                               "no visible window matches " + match.app );
        }
        return last_match;
    }

    grab::Result<WindowRect>
    window_geometry( const XcbConnection& conn,
                     const WindowRef&     window )
    {
        if( !window.valid )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "window reference is invalid" );
        }

        auto geometry = read_geometry( conn, window.id );
        if( !geometry.has_value() )
        {
            return grab::fail( geometry.error().code, geometry.error().message );
        }
        auto origin = translate_origin( conn, window.id );
        if( !origin.has_value() )
        {
            return grab::fail( origin.error().code, origin.error().message );
        }

        return WindowRect{
            .x      = origin->dst_x,
            .y      = origin->dst_y,
            .width  = geometry->width,
            .height = geometry->height,
        };
    }

}    // namespace grab::platform::x11
