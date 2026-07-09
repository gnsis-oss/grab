#include "grab/result.hpp"
#include "input/locator.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
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

namespace grab::input
{

    namespace
    {

        constexpr int              kXcbOk                = 0;
        constexpr std::uint32_t    kPropertyOffsetZero   = 0U;
        constexpr std::uint32_t    kPropertyLengthProbe  = 0U;
        constexpr std::uint32_t    kBytesPerPropertyUnit = 4U;
        constexpr std::uint32_t    kMaxPropertyBytes     = 1U * 1'024U * 1'024U;
        constexpr std::uint8_t     kFormat8Bits          = 8U;
        constexpr std::uint8_t     kFormat32Bits         = 32U;
        constexpr std::string_view kNetClientListAtom    = "_NET_CLIENT_LIST";
        constexpr std::string_view kNetWmNameAtom        = "_NET_WM_NAME";
        constexpr std::string_view kUtf8StringAtom       = "UTF8_STRING";
        constexpr std::string_view kLocatorContext       = "XCB window locator";

        template<typename T>
        using XcbOwned = std::unique_ptr<T, decltype( &std::free )>;

        using XcbConnection =
            std::unique_ptr<xcb_connection_t, decltype( &xcb_disconnect )>;

        struct Atoms
        {
                xcb_atom_t net_client_list = XCB_ATOM_NONE;
                xcb_atom_t net_wm_name     = XCB_ATOM_NONE;
                xcb_atom_t utf8_string     = XCB_ATOM_NONE;
        };

        struct PropertyData
        {
                xcb_atom_t             type   = XCB_ATOM_NONE;
                std::uint8_t           format = 0U;
                std::vector<std::byte> bytes;
        };

        struct WindowClass
        {
                std::string instance;
                std::string class_name;
        };

        template<typename T>
        [[nodiscard]]
        XcbOwned<T>
        take_xcb_owned( T* pointer ) noexcept
        {
            return XcbOwned<T>{ pointer, &std::free };
        }

        [[nodiscard]]
        grab::Result<void>
        fail_if_connection_closed( const xcb_connection_t* connection )
        {
            if( connection == nullptr )
            {
                return grab::fail( grab::ErrorCode::device_inaccessible,
                                   "XCB window locator connection is not open" );
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<std::uint32_t>
        default_root( xcb_connection_t* connection,
                      int               screen_index )
        {
            xcb_screen_iterator_t iterator =
                xcb_setup_roots_iterator( xcb_get_setup( connection ) );
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

            return iterator.data->root;
        }

        [[nodiscard]]
        grab::Result<xcb_atom_t>
        intern_atom( xcb_connection_t* connection,
                     std::string_view  name,
                     bool              only_if_exists )
        {
            xcb_generic_error_t* raw_error = nullptr;
            const auto           reply     = take_xcb_owned( xcb_intern_atom_reply(
                connection,
                xcb_intern_atom( connection,
                                 only_if_exists ? 1U : 0U,
                                 static_cast<std::uint16_t>( name.size() ),
                                 name.data() ),
                &raw_error
            ) );
            const auto           error     = take_xcb_owned( raw_error );
            if( error != nullptr || reply == nullptr )
            {
                return grab::fail( grab::ErrorCode::protocol_error,
                                   std::string{ kLocatorContext } +
                                       " atom lookup failed for " +
                                       std::string{ name } );
            }

            return reply->atom;
        }

        [[nodiscard]]
        grab::Result<Atoms>
        intern_atoms( xcb_connection_t* connection )
        {
            auto net_client_list = intern_atom( connection, kNetClientListAtom, true );
            if( !net_client_list.has_value() )
            {
                return std::unexpected( std::move( net_client_list.error() ) );
            }

            auto net_wm_name = intern_atom( connection, kNetWmNameAtom, true );
            if( !net_wm_name.has_value() )
            {
                return std::unexpected( std::move( net_wm_name.error() ) );
            }

            auto utf8_string = intern_atom( connection, kUtf8StringAtom, true );
            if( !utf8_string.has_value() )
            {
                return std::unexpected( std::move( utf8_string.error() ) );
            }

            return Atoms{
                .net_client_list = *net_client_list,
                .net_wm_name     = *net_wm_name,
                .utf8_string     = *utf8_string,
            };
        }

        [[nodiscard]]
        bool
        stale_window_error( const xcb_generic_error_t& error ) noexcept
        {
            return error.error_code == XCB_WINDOW;
        }

        [[nodiscard]]
        grab::Result<std::optional<PropertyData>>
        read_property( xcb_connection_t* connection,
                       xcb_window_t      window,
                       xcb_atom_t        property,
                       xcb_atom_t        type )
        {
            if( property == XCB_ATOM_NONE )
            {
                return std::optional<PropertyData>{};
            }

            xcb_generic_error_t* raw_probe_error = nullptr;
            const auto           probe_reply     = take_xcb_owned(
                xcb_get_property_reply( connection,
                                        xcb_get_property( connection,
                                                          0U,
                                                          window,
                                                          property,
                                                          type,
                                                          kPropertyOffsetZero,
                                                          kPropertyLengthProbe ),
                                        &raw_probe_error )
            );
            const auto probe_error = take_xcb_owned( raw_probe_error );
            if( probe_error != nullptr )
            {
                if( stale_window_error( *probe_error ) )
                {
                    return std::optional<PropertyData>{};
                }
                return grab::fail( grab::ErrorCode::protocol_error,
                                   "XCB property probe failed" );
            }
            if( probe_reply == nullptr || probe_reply->type == XCB_ATOM_NONE )
            {
                return std::optional<PropertyData>{};
            }
            if( probe_reply->bytes_after > kMaxPropertyBytes )
            {
                return grab::fail( grab::ErrorCode::protocol_error,
                                   "XCB property exceeds locator read limit" );
            }

            const std::uint32_t total_units =
                ( probe_reply->bytes_after + kBytesPerPropertyUnit - 1U ) /
                kBytesPerPropertyUnit;

            xcb_generic_error_t* raw_value_error = nullptr;
            const auto           value_reply     = take_xcb_owned(
                xcb_get_property_reply( connection,
                                        xcb_get_property( connection,
                                                          0U,
                                                          window,
                                                          property,
                                                          type,
                                                          kPropertyOffsetZero,
                                                          total_units ),
                                        &raw_value_error )
            );
            const auto value_error = take_xcb_owned( raw_value_error );
            if( value_error != nullptr )
            {
                if( stale_window_error( *value_error ) )
                {
                    return std::optional<PropertyData>{};
                }
                return grab::fail( grab::ErrorCode::protocol_error,
                                   "XCB property read failed" );
            }
            if( value_reply == nullptr || value_reply->type == XCB_ATOM_NONE )
            {
                return std::optional<PropertyData>{};
            }

            const int value_length = xcb_get_property_value_length( value_reply.get() );
            if( value_length <= 0 )
            {
                return PropertyData{
                    .type   = value_reply->type,
                    .format = value_reply->format,
                    .bytes  = {},
                };
            }

            const auto* const begin = static_cast<const std::byte*>(
                xcb_get_property_value( value_reply.get() )
            );
            const auto length = static_cast<std::size_t>( value_length );
            const std::span<const std::byte> values{ begin, length };
            std::vector<std::byte>           bytes;
            bytes.assign( values.begin(), values.end() );

            return PropertyData{
                .type   = value_reply->type,
                .format = value_reply->format,
                .bytes  = std::move( bytes ),
            };
        }

        [[nodiscard]]
        std::string
        bytes_to_string( const std::vector<std::byte>& bytes )
        {
            std::string text;
            text.reserve( bytes.size() );
            std::ranges::transform( bytes,
                                    std::back_inserter( text ),
                                    []( std::byte value )
                                    {
                                        return static_cast<char>( value );
                                    } );
            return text;
        }

        [[nodiscard]]
        std::optional<WindowClass>
        parse_wm_class( const PropertyData& property )
        {
            if( property.format != kFormat8Bits || property.bytes.empty() )
            {
                return std::nullopt;
            }

            const std::string value = bytes_to_string( property.bytes );
            const auto        split = value.find( '\0' );
            if( split == std::string::npos )
            {
                return WindowClass{
                    .instance   = value,
                    .class_name = {},
                };
            }

            const auto class_begin = split + 1U;
            const auto class_end   = value.find( '\0', class_begin );
            return WindowClass{
                .instance   = value.substr( 0U, split ),
                .class_name = value.substr( class_begin,
                                            class_end == std::string::npos
                                                ? std::string::npos
                                                : class_end - class_begin ),
            };
        }

        [[nodiscard]]
        std::string
        ascii_lower( std::string_view text )
        {
            std::string lowered;
            lowered.reserve( text.size() );
            std::ranges::transform(
                text,
                std::back_inserter( lowered ),
                []( unsigned char value )
                {
                    return static_cast<char>( std::tolower( value ) );
                }
            );
            return lowered;
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

            const std::string lower_haystack = ascii_lower( haystack );
            const std::string lower_needle   = ascii_lower( needle );
            return lower_haystack.contains( lower_needle );
        }

        [[nodiscard]]
        bool
        wm_class_matches( const WindowClass&              window_class,
                          const std::vector<std::string>& candidates )
        {
            return std::ranges::any_of(
                candidates,
                [&]( const std::string& candidate )
                {
                    return !candidate.empty() &&
                           ( contains_case_insensitive( window_class.instance,
                                                        candidate ) ||
                             contains_case_insensitive( window_class.class_name,
                                                        candidate ) );
                }
            );
        }

        [[nodiscard]]
        grab::Result<std::optional<WindowClass>>
        window_class_for( xcb_connection_t* connection,
                          xcb_window_t      window )
        {
            auto property =
                read_property( connection, window, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING );
            if( !property.has_value() )
            {
                return std::unexpected( std::move( property.error() ) );
            }
            if( !property->has_value() )
            {
                return std::optional<WindowClass>{};
            }
            return parse_wm_class( **property );
        }

        [[nodiscard]]
        grab::Result<std::string>
        window_title_for( xcb_connection_t* connection,
                          xcb_window_t      window,
                          const Atoms&      atoms )
        {
            if( atoms.net_wm_name !=
                XCB_ATOM_NONE &&
                atoms.utf8_string != XCB_ATOM_NONE )
            {
                auto net_wm_name = read_property( connection,
                                                  window,
                                                  atoms.net_wm_name,
                                                  atoms.utf8_string );
                if( !net_wm_name.has_value() )
                {
                    return std::unexpected( std::move( net_wm_name.error() ) );
                }
                if( net_wm_name->has_value() &&
                    ( *net_wm_name )->format == kFormat8Bits )
                {
                    return bytes_to_string( ( *net_wm_name )->bytes );
                }
            }

            auto wm_name =
                read_property( connection, window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING );
            if( !wm_name.has_value() )
            {
                return std::unexpected( std::move( wm_name.error() ) );
            }
            if( wm_name->has_value() && ( *wm_name )->format == kFormat8Bits )
            {
                return bytes_to_string( ( *wm_name )->bytes );
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<std::vector<xcb_window_t>>
        net_client_list_windows( xcb_connection_t* connection,
                                 xcb_window_t      root,
                                 xcb_atom_t        net_client_list )
        {
            auto property =
                read_property( connection, root, net_client_list, XCB_ATOM_WINDOW );
            if( !property.has_value() )
            {
                return std::unexpected( std::move( property.error() ) );
            }
            if( !property->has_value() ||
                ( *property )->format !=
                kFormat32Bits ||
                ( *property )->bytes.empty() )
            {
                return std::vector<xcb_window_t>{};
            }

            const auto window_count =
                ( *property )->bytes.size() / sizeof( xcb_window_t );

            std::vector<xcb_window_t> result;
            result.reserve( window_count );
            const std::span<const std::byte> property_bytes{ ( *property )->bytes };
            for( std::size_t index = 0U; index < window_count; ++index )
            {
                std::array<std::byte, sizeof( xcb_window_t )> raw_window{};
                const auto byte_offset = index * raw_window.size();
                const auto window_bytes =
                    property_bytes.subspan( byte_offset, raw_window.size() );
                std::ranges::copy( window_bytes, raw_window.begin() );
                result.push_back( std::bit_cast<xcb_window_t>( raw_window ) );
            }
            return result;
        }

        [[nodiscard]]
        grab::Result<std::vector<xcb_window_t>>
        root_tree_windows( xcb_connection_t* connection,
                           xcb_window_t      root )
        {
            xcb_generic_error_t* raw_error = nullptr;
            const auto           reply =
                take_xcb_owned( xcb_query_tree_reply( connection,
                                                      xcb_query_tree( connection, root ),
                                                      &raw_error ) );
            const auto error = take_xcb_owned( raw_error );
            if( error != nullptr || reply == nullptr )
            {
                return grab::fail( grab::ErrorCode::protocol_error,
                                   "XCB root tree query failed" );
            }

            const int child_count = xcb_query_tree_children_length( reply.get() );
            if( child_count <= 0 )
            {
                return std::vector<xcb_window_t>{};
            }

            const auto count = static_cast<std::size_t>( child_count );
            const std::span<const xcb_window_t> children{
                xcb_query_tree_children( reply.get() ),
                count,
            };
            std::vector<xcb_window_t> windows;
            windows.reserve( count );
            for( const xcb_window_t child : children )
            {
                windows.push_back( child );
            }
            return windows;
        }

        [[nodiscard]]
        grab::Result<std::vector<xcb_window_t>>
        top_level_windows( xcb_connection_t* connection,
                           xcb_window_t      root,
                           const Atoms&      atoms )
        {
            auto client_list =
                net_client_list_windows( connection, root, atoms.net_client_list );
            if( !client_list.has_value() )
            {
                return std::unexpected( std::move( client_list.error() ) );
            }
            if( !client_list->empty() )
            {
                return client_list;
            }
            return root_tree_windows( connection, root );
        }

        [[nodiscard]]
        LocatedWindow
        located_geometry( xcb_connection_t* connection,
                          xcb_window_t      root,
                          xcb_window_t      window )
        {
            LocatedWindow located{
                .window = window,
                .x      = 0,
                .y      = 0,
                .width  = 0U,
                .height = 0U,
                .trust  = GeometryTrust::unavailable,
            };

            xcb_generic_error_t* raw_geometry_error = nullptr;
            const auto           geometry           = take_xcb_owned(
                xcb_get_geometry_reply( connection,
                                        xcb_get_geometry( connection, window ),
                                        &raw_geometry_error )
            );
            const auto geometry_error = take_xcb_owned( raw_geometry_error );
            if( geometry_error != nullptr || geometry == nullptr )
            {
                return located;
            }

            located.x                                = geometry->x;
            located.y                                = geometry->y;
            located.width                            = geometry->width;
            located.height                           = geometry->height;
            located.trust                            = GeometryTrust::estimated;

            xcb_generic_error_t* raw_translate_error = nullptr;
            const auto translation     = take_xcb_owned( xcb_translate_coordinates_reply(
                connection,
                xcb_translate_coordinates( connection, window, root, 0, 0 ),
                &raw_translate_error
            ) );
            const auto translate_error = take_xcb_owned( raw_translate_error );
            if( translate_error != nullptr || translation == nullptr )
            {
                return located;
            }

            located.x     = translation->dst_x;
            located.y     = translation->dst_y;
            located.trust = GeometryTrust::trusted;
            return located;
        }

    }    // namespace

    WindowLocator::WindowLocator( xcb_connection_t* connection,
                                  std::uint32_t     root ) noexcept :
        connection_( connection ),
        root_( root )
    {
    }

    WindowLocator::~WindowLocator()
    {
        if( connection_ != nullptr )
        {
            xcb_disconnect( connection_ );
        }
    }

    WindowLocator::WindowLocator( WindowLocator&& other ) noexcept :
        connection_( std::exchange( other.connection_,
                                    nullptr ) ),
        root_( std::exchange( other.root_,
                              0U ) )
    {
    }

    WindowLocator&
    WindowLocator::operator=( WindowLocator&& other ) noexcept
    {
        if( this != &other )
        {
            if( connection_ != nullptr )
            {
                xcb_disconnect( connection_ );
            }
            connection_ = std::exchange( other.connection_, nullptr );
            root_       = std::exchange( other.root_, 0U );
        }
        return *this;
    }

    grab::Result<WindowLocator>
    WindowLocator::open( const char* display )
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

        auto root_result = default_root( connection.get(), screen_index );
        if( !root_result.has_value() )
        {
            return std::unexpected( std::move( root_result.error() ) );
        }

        return WindowLocator{ connection.release(), *root_result };
    }

    grab::Result<LocatedWindow>
    WindowLocator::locate( const std::vector<std::string>& wm_class_candidates,
                           std::string_view                title )
    {
        auto open_result = fail_if_connection_closed( connection_ );
        if( !open_result.has_value() )
        {
            return std::unexpected( std::move( open_result.error() ) );
        }

        auto atoms = intern_atoms( connection_ );
        if( !atoms.has_value() )
        {
            return std::unexpected( std::move( atoms.error() ) );
        }

        auto windows = top_level_windows( connection_, root_, *atoms );
        if( !windows.has_value() )
        {
            return std::unexpected( std::move( windows.error() ) );
        }

        for( const xcb_window_t window : *windows )
        {
            auto window_class = window_class_for( connection_, window );
            if( !window_class.has_value() )
            {
                return std::unexpected( std::move( window_class.error() ) );
            }
            if( !window_class->has_value() ||
                !wm_class_matches( **window_class, wm_class_candidates ) )
            {
                continue;
            }

            if( !title.empty() )
            {
                auto window_title = window_title_for( connection_, window, *atoms );
                if( !window_title.has_value() )
                {
                    return std::unexpected( std::move( window_title.error() ) );
                }
                if( !contains_case_insensitive( *window_title, title ) )
                {
                    continue;
                }
            }

            return located_geometry( connection_, root_, window );
        }

        return grab::fail( grab::ErrorCode::window_not_found,
                           "No top-level X11 window matched the requested WM_CLASS" );
    }

}    // namespace grab::input
