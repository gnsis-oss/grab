#include "core/ascii.hpp"
#include "grab/result.hpp"
#include "input/locator.hpp"
#include "platform/x11/protocol.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
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
#include <thread>
#include <utility>
#include <vector>
#include <xcb/xcb.h>
#include <xcb/xproto.h>

namespace grab::input
{

    namespace
    {

        constexpr int              xcbOk                = 0;
        constexpr std::uint32_t    propertyOffsetZero   = 0U;
        constexpr std::uint32_t    propertyLengthProbe  = 0U;
        constexpr std::uint32_t    bytesPerPropertyUnit = 4U;
        constexpr std::uint32_t    maxPropertyBytes     = 1U * 1'024U * 1'024U;
        constexpr std::uint8_t     format8Bits          = 8U;
        constexpr std::uint8_t     format32Bits         = 32U;
        constexpr std::uint8_t     doNotPropagate       = 0U;
        constexpr std::uint32_t    activeSourceNormal   = 1U;
        constexpr std::uint32_t    noCurrentWindow      = 0U;
        constexpr std::uint32_t    stackAbove           = XCB_STACK_MODE_ABOVE;
        constexpr std::string_view locatorContext       = "XCB window locator";
        constexpr auto             activationTimeout    = std::chrono::seconds{ 1 };
        constexpr auto          activationPollInterval = std::chrono::milliseconds{ 10 };

        constexpr std::uint32_t activeWindowEventMask =
            static_cast<std::uint32_t>( XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY ) |
            static_cast<std::uint32_t>( XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT );

        template<typename T>
        using XcbOwned = std::unique_ptr<T, decltype( &std::free )>;

        using XcbConnection =
            std::unique_ptr<xcb_connection_t, decltype( &xcb_disconnect )>;

        struct Atoms
        {
                xcb_atom_t net_client_list          = XCB_ATOM_NONE;
                xcb_atom_t net_client_list_stacking = XCB_ATOM_NONE;
                xcb_atom_t net_wm_name              = XCB_ATOM_NONE;
                xcb_atom_t utf8_string              = XCB_ATOM_NONE;
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
                return grab::fail( grab::ErrorCode::DeviceInaccessible,
                                   "XCB window locator connection is not open" );
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        check_request( xcb_connection_t* connection,
                       xcb_void_cookie_t cookie,
                       std::string_view  operation )
        {
            const auto error = take_xcb_owned( xcb_request_check( connection, cookie ) );
            if( error != nullptr )
            {
                return grab::fail( grab::ErrorCode::ProtocolError,
                                   std::string{ operation } + " failed" );
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
                return grab::fail( grab::ErrorCode::DeviceInaccessible,
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
                return grab::fail( grab::ErrorCode::ProtocolError,
                                   std::string{ locatorContext } +
                                       " atom lookup failed for " +
                                       std::string{ name } );
            }

            return reply->atom;
        }

        [[nodiscard]]
        grab::Result<Atoms>
        intern_atoms( xcb_connection_t* connection )
        {
            auto net_client_list_stacking =
                intern_atom( connection,
                             grab::platform::x11::atom_name::netClientListStacking,
                             true );
            if( !net_client_list_stacking.has_value() )
            {
                return std::unexpected( std::move( net_client_list_stacking.error() ) );
            }

            auto net_client_list =
                intern_atom( connection,
                             grab::platform::x11::atom_name::netClientList,
                             true );
            if( !net_client_list.has_value() )
            {
                return std::unexpected( std::move( net_client_list.error() ) );
            }

            auto net_wm_name = intern_atom( connection,
                                            grab::platform::x11::atom_name::netWmName,
                                            true );
            if( !net_wm_name.has_value() )
            {
                return std::unexpected( std::move( net_wm_name.error() ) );
            }

            auto utf8_string = intern_atom( connection,
                                            grab::platform::x11::atom_name::utf8String,
                                            true );
            if( !utf8_string.has_value() )
            {
                return std::unexpected( std::move( utf8_string.error() ) );
            }

            return Atoms{
                .net_client_list          = *net_client_list,
                .net_client_list_stacking = *net_client_list_stacking,
                .net_wm_name              = *net_wm_name,
                .utf8_string              = *utf8_string,
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
                                                          propertyOffsetZero,
                                                          propertyLengthProbe ),
                                        &raw_probe_error )
            );
            const auto probe_error = take_xcb_owned( raw_probe_error );
            if( probe_error != nullptr )
            {
                if( stale_window_error( *probe_error ) )
                {
                    return std::optional<PropertyData>{};
                }
                return grab::fail( grab::ErrorCode::ProtocolError,
                                   "XCB property probe failed" );
            }
            if( probe_reply == nullptr || probe_reply->type == XCB_ATOM_NONE )
            {
                return std::optional<PropertyData>{};
            }
            if( probe_reply->bytes_after > maxPropertyBytes )
            {
                return grab::fail( grab::ErrorCode::ProtocolError,
                                   "XCB property exceeds locator read limit" );
            }

            const std::uint32_t total_units =
                ( probe_reply->bytes_after + bytesPerPropertyUnit - 1U ) /
                bytesPerPropertyUnit;

            xcb_generic_error_t* raw_value_error = nullptr;
            const auto           value_reply     = take_xcb_owned(
                xcb_get_property_reply( connection,
                                        xcb_get_property( connection,
                                                          0U,
                                                          window,
                                                          property,
                                                          type,
                                                          propertyOffsetZero,
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
                return grab::fail( grab::ErrorCode::ProtocolError,
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
            if( property.format != format8Bits || property.bytes.empty() )
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
        bool
        wm_class_matches( const WindowClass&              window_class,
                          const std::vector<std::string>& candidates )
        {
            return std::ranges::any_of(
                candidates,
                [&]( const std::string& candidate )
                {
                    return !candidate.empty() &&
                           ( grab::core::ascii_icontains( window_class.instance,
                                                          candidate ) ||
                             grab::core::ascii_icontains( window_class.class_name,
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
                if( net_wm_name->has_value() && ( *net_wm_name )->format == format8Bits )
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
            if( wm_name->has_value() && ( *wm_name )->format == format8Bits )
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
                format32Bits ||
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

        void
        prefer_topmost_windows( std::vector<xcb_window_t>& windows )
        {
            std::ranges::reverse( windows );
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
                return grab::fail( grab::ErrorCode::ProtocolError,
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
            prefer_topmost_windows( windows );
            return windows;
        }

        [[nodiscard]]
        grab::Result<std::vector<xcb_window_t>>
        top_level_windows( xcb_connection_t* connection,
                           xcb_window_t      root,
                           const Atoms&      atoms )
        {
            auto stacking_list =
                net_client_list_windows( connection,
                                         root,
                                         atoms.net_client_list_stacking );
            if( !stacking_list.has_value() )
            {
                return std::unexpected( std::move( stacking_list.error() ) );
            }
            prefer_topmost_windows( *stacking_list );

            auto client_list =
                net_client_list_windows( connection, root, atoms.net_client_list );
            if( !client_list.has_value() )
            {
                return std::unexpected( std::move( client_list.error() ) );
            }
            prefer_topmost_windows( *client_list );

            stacking_list->reserve( stacking_list->size() + client_list->size() );
            for( const xcb_window_t window : *client_list )
            {
                if( std::ranges::find( *stacking_list, window ) == stacking_list->end() )
                {
                    stacking_list->push_back( window );
                }
            }
            if( !stacking_list->empty() )
            {
                return stacking_list;
            }
            return root_tree_windows( connection, root );
        }

        [[nodiscard]]
        grab::Result<bool>
        window_is_viewable( xcb_connection_t* connection,
                            xcb_window_t      window )
        {
            xcb_generic_error_t* raw_error = nullptr;
            const auto           reply = take_xcb_owned( xcb_get_window_attributes_reply(
                connection,
                xcb_get_window_attributes( connection, window ),
                &raw_error
            ) );
            const auto           error = take_xcb_owned( raw_error );
            if( error != nullptr )
            {
                if( stale_window_error( *error ) )
                {
                    return false;
                }
                return grab::fail( grab::ErrorCode::ProtocolError,
                                   "XCB window attributes query failed" );
            }
            if( reply == nullptr )
            {
                return grab::fail( grab::ErrorCode::ProtocolError,
                                   "XCB window attributes reply is unavailable" );
            }
            return reply->map_state == XCB_MAP_STATE_VIEWABLE;
        }

        [[nodiscard]]
        LocatedWindow
        located_geometry( xcb_connection_t* connection,
                          xcb_window_t      root,
                          xcb_window_t      window )
        {
            LocatedWindow located{};
            located.window                          = window;

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

            located.bounds.x                         = geometry->x;
            located.bounds.y                         = geometry->y;
            located.bounds.width                     = geometry->width;
            located.bounds.height                    = geometry->height;
            located.trust                            = GeometryTrust::Estimated;

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

            located.bounds.x = translation->dst_x;
            located.bounds.y = translation->dst_y;
            located.trust    = GeometryTrust::Trusted;
            return located;
        }

        [[nodiscard]]
        grab::Result<xcb_window_t>
        input_focus( xcb_connection_t* connection )
        {
            xcb_generic_error_t* raw_error = nullptr;
            const auto           reply     = take_xcb_owned(
                xcb_get_input_focus_reply( connection,
                                           xcb_get_input_focus( connection ),
                                           &raw_error )
            );
            const auto error = take_xcb_owned( raw_error );
            if( error != nullptr || reply == nullptr )
            {
                return grab::fail( grab::ErrorCode::ProtocolError,
                                   "X11 input-focus query failed" );
            }
            return reply->focus;
        }

        [[nodiscard]]
        grab::Result<bool>
        target_owns_focus( xcb_connection_t*                           connection,
                           xcb_window_t                                target,
                           xcb_window_t                                focus,
                           const std::chrono::steady_clock::time_point deadline )
        {
            if( focus == target )
            {
                return true;
            }
            if( focus == XCB_INPUT_FOCUS_NONE || focus == XCB_INPUT_FOCUS_POINTER_ROOT )
            {
                return false;
            }

            xcb_window_t current = focus;
            while( std::chrono::steady_clock::now() < deadline )
            {
                xcb_generic_error_t* raw_error = nullptr;
                const auto           reply     = take_xcb_owned(
                    xcb_query_tree_reply( connection,
                                          xcb_query_tree( connection, current ),
                                          &raw_error )
                );
                const auto error = take_xcb_owned( raw_error );
                if( error != nullptr )
                {
                    if( stale_window_error( *error ) )
                    {
                        return false;
                    }
                    return grab::fail( grab::ErrorCode::ProtocolError,
                                       "X11 focus ancestry query failed" );
                }
                if( reply == nullptr )
                {
                    return grab::fail( grab::ErrorCode::ProtocolError,
                                       "X11 focus ancestry reply is unavailable" );
                }

                const xcb_window_t parent = reply->parent;
                if( parent == target )
                {
                    return true;
                }
                if( parent ==
                    XCB_WINDOW_NONE ||
                    parent ==
                    current ||
                    current == reply->root )
                {
                    return false;
                }
                current = parent;
            }
            return false;
        }

        [[nodiscard]]
        grab::Result<void>
        wait_for_target_focus( xcb_connection_t* connection,
                               xcb_window_t      target )
        {
            const auto deadline = std::chrono::steady_clock::now() + activationTimeout;
            while( true )
            {
                auto focus = input_focus( connection );
                if( !focus.has_value() )
                {
                    return std::unexpected( std::move( focus.error() ) );
                }

                auto owns_focus =
                    target_owns_focus( connection, target, *focus, deadline );
                if( !owns_focus.has_value() )
                {
                    return std::unexpected( std::move( owns_focus.error() ) );
                }
                if( *owns_focus )
                {
                    return {};
                }
                if( std::chrono::steady_clock::now() >= deadline )
                {
                    return grab::fail( grab::ErrorCode::ProviderFailed,
                                       "X11 window activation did not acquire input "
                                       "focus before timeout" );
                }
                std::this_thread::sleep_for( activationPollInterval );
            }
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
            xcb_connection_has_error( connection.get() ) != xcbOk )
        {
            return grab::fail( grab::ErrorCode::DeviceInaccessible,
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
            auto viewable = window_is_viewable( connection_, window );
            if( !viewable.has_value() )
            {
                return std::unexpected( std::move( viewable.error() ) );
            }
            if( !*viewable )
            {
                continue;
            }

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
                if( !grab::core::ascii_icontains( *window_title, title ) )
                {
                    continue;
                }
            }

            return located_geometry( connection_, root_, window );
        }

        return grab::fail( grab::ErrorCode::WindowNotFound,
                           "No top-level X11 window matched the requested WM_CLASS" );
    }

    grab::Result<void>
    WindowLocator::activate( const LocatedWindow& window )
    {
        auto open_result = fail_if_connection_closed( connection_ );
        if( !open_result.has_value() )
        {
            return std::unexpected( std::move( open_result.error() ) );
        }
        if( window.window == XCB_WINDOW_NONE )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "cannot activate an empty X11 window" );
        }

        auto atom = intern_atom( connection_,
                                 grab::platform::x11::atom_name::netActiveWindow,
                                 false );
        if( !atom.has_value() )
        {
            return std::unexpected( std::move( atom.error() ) );
        }

        const xcb_client_message_event_t event{
            .response_type = XCB_CLIENT_MESSAGE,
            .format        = format32Bits,
            .sequence      = 0U,
            .window        = window.window,
            .type          = *atom,
            .data          = xcb_client_message_data_t{
                                                       .data32 = {
                    activeSourceNormal,
                    XCB_CURRENT_TIME,
                    noCurrentWindow,
                }, },
        };
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        const auto* const raw_event = reinterpret_cast<const char*>( &event );
        auto send_result = check_request( connection_,
                                          xcb_send_event_checked( connection_,
                                                                  doNotPropagate,
                                                                  root_,
                                                                  activeWindowEventMask,
                                                                  raw_event ),
                                          "X11 active-window request" );
        if( !send_result.has_value() )
        {
            return send_result;
        }

        auto stack_result =
            check_request( connection_,
                           xcb_configure_window_checked( connection_,
                                                         window.window,
                                                         XCB_CONFIG_WINDOW_STACK_MODE,
                                                         &stackAbove ),
                           "X11 window raise request" );
        if( !stack_result.has_value() )
        {
            return stack_result;
        }

        if( xcb_flush( connection_ ) <= xcbOk )
        {
            return grab::fail( grab::ErrorCode::ProtocolError,
                               "X11 window activation flush failed" );
        }

        return wait_for_target_focus( connection_, window.window );
    }

}    // namespace grab::input
