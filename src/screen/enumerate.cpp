#include "grab/geometry/rectangle.hpp"
#include "grab/result.hpp"
#include "platform/x11/protocol.hpp"
#include "screen/enumerate.hpp"

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
#include <xcb/randr.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>

namespace grab::screen
{
    namespace
    {

        constexpr int              xcbOk                 = 0;
        constexpr std::uint8_t     x11SuccessResponse    = 1U;
        constexpr std::uint32_t    propertyOffsetZero    = 0U;
        constexpr std::uint32_t    propertyLengthProbe   = 0U;
        constexpr std::uint32_t    bytesPerPropertyUnit  = 4U;
        constexpr std::uint32_t    maxPropertyBytes      = 1U * 1'024U * 1'024U;
        constexpr std::uint8_t     format8Bits           = 8U;
        constexpr std::uint8_t     format32Bits          = 32U;
        constexpr std::int16_t     rootOrigin            = 0;
        constexpr std::string_view randrExtensionName    = "RANDR";
        constexpr std::string_view fallbackOutputName    = "screen";
        constexpr std::string_view generatedOutputPrefix = "output-";

        template<typename T>
        using XcbOwned = std::unique_ptr<T, decltype( &std::free )>;

        using XcbConnection =
            std::unique_ptr<xcb_connection_t, decltype( &xcb_disconnect )>;

        struct ScreenInfo
        {
                xcb_window_t  root   = XCB_WINDOW_NONE;
                std::uint16_t width  = 0U;
                std::uint16_t height = 0U;
        };

        struct ConnectedDisplay
        {
                XcbConnection connection;
                ScreenInfo    screen;
        };

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

        using Geometry = grab::geometry::Rectangle;

        template<typename T>
        [[nodiscard]]
        XcbOwned<T>
        take_xcb_owned( T* pointer ) noexcept
        {
            return XcbOwned<T>{ pointer, &std::free };
        }

        [[nodiscard]]
        bool
        stale_window_error( const xcb_generic_error_t& error ) noexcept
        {
            return error.error_code == XCB_WINDOW || error.error_code == XCB_DRAWABLE;
        }

        [[nodiscard]]
        grab::Result<ScreenInfo>
        default_screen_info( xcb_connection_t* connection,
                             int               screen_index )
        {
            const xcb_setup_t* const setup = xcb_get_setup( connection );
            if( setup == nullptr || setup->status != x11SuccessResponse )
            {
                return grab::fail( grab::ErrorCode::DeviceInaccessible,
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
                return grab::fail( grab::ErrorCode::DeviceInaccessible,
                                   "XCB default screen is unavailable" );
            }

            return ScreenInfo{
                .root   = iterator.data->root,
                .width  = iterator.data->width_in_pixels,
                .height = iterator.data->height_in_pixels,
            };
        }

        [[nodiscard]]
        grab::Result<ConnectedDisplay>
        connect_display( const char* display )
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

            auto screen = default_screen_info( connection.get(), screen_index );
            if( !screen.has_value() )
            {
                return std::unexpected( std::move( screen.error() ) );
            }

            return ConnectedDisplay{
                .connection = std::move( connection ),
                .screen     = *screen,
            };
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
                                   "XCB atom lookup failed for " + std::string{ name } );
            }

            return reply->atom;
        }

        [[nodiscard]]
        grab::Result<Atoms>
        intern_atoms( xcb_connection_t* connection )
        {
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
                .net_client_list = *net_client_list,
                .net_wm_name     = *net_wm_name,
                .utf8_string     = *utf8_string,
            };
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
                                   "XCB property exceeds enumeration read limit" );
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
            if( value_length < 0 )
            {
                return grab::fail( grab::ErrorCode::ProtocolError,
                                   "XCB property returned a negative length" );
            }
            if( value_length == 0 )
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
        std::string
        trim_trailing_nulls( std::string text )
        {
            while( !text.empty() && text.back() == '\0' )
            {
                text.pop_back();
            }
            return text;
        }

        [[nodiscard]]
        std::string
        parse_wm_class( const PropertyData& property )
        {
            if( property.format != format8Bits || property.bytes.empty() )
            {
                return {};
            }

            std::string value = bytes_to_string( property.bytes );
            const auto  split = value.find( '\0' );
            if( split == std::string::npos )
            {
                return value;
            }

            std::string instance    = value.substr( 0U, split );
            const auto  class_begin = split + 1U;
            if( class_begin >= value.size() )
            {
                return instance;
            }

            const auto class_end = value.find( '\0', class_begin );
            auto       class_name =
                value.substr( class_begin,
                              class_end == std::string::npos ? std::string::npos
                                                             : class_end - class_begin );
            if( class_name.empty() )
            {
                return instance;
            }
            return class_name;
        }

        [[nodiscard]]
        grab::Result<std::string>
        read_text_property( xcb_connection_t* connection,
                            xcb_window_t      window,
                            xcb_atom_t        property,
                            xcb_atom_t        type )
        {
            auto property_data = read_property( connection, window, property, type );
            if( !property_data.has_value() )
            {
                return std::unexpected( std::move( property_data.error() ) );
            }
            if( !property_data->has_value() ||
                ( *property_data )->format != format8Bits )
            {
                return {};
            }

            return trim_trailing_nulls( bytes_to_string( ( *property_data )->bytes ) );
        }

        [[nodiscard]]
        grab::Result<std::string>
        read_wm_class( xcb_connection_t* connection,
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
                return {};
            }
            return parse_wm_class( **property );
        }

        [[nodiscard]]
        grab::Result<std::string>
        read_title( xcb_connection_t* connection,
                    xcb_window_t      window,
                    const Atoms&      atoms )
        {
            if( atoms.net_wm_name !=
                XCB_ATOM_NONE &&
                atoms.utf8_string != XCB_ATOM_NONE )
            {
                auto net_wm_name = read_text_property( connection,
                                                       window,
                                                       atoms.net_wm_name,
                                                       atoms.utf8_string );
                if( !net_wm_name.has_value() )
                {
                    return std::unexpected( std::move( net_wm_name.error() ) );
                }
                if( !net_wm_name->empty() )
                {
                    return *net_wm_name;
                }
            }

            return read_text_property( connection,
                                       window,
                                       XCB_ATOM_WM_NAME,
                                       XCB_ATOM_STRING );
        }

        [[nodiscard]]
        grab::Result<std::vector<xcb_window_t>>
        client_list_windows( xcb_connection_t* connection,
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
            std::vector<xcb_window_t> windows;
            windows.reserve( window_count );

            const std::span<const std::byte> property_bytes{ ( *property )->bytes };
            for( std::size_t index = 0U; index < window_count; ++index )
            {
                std::array<std::byte, sizeof( xcb_window_t )> raw_window{};
                const auto byte_offset = index * raw_window.size();
                const auto window_bytes =
                    property_bytes.subspan( byte_offset, raw_window.size() );
                std::ranges::copy( window_bytes, raw_window.begin() );
                windows.push_back( std::bit_cast<xcb_window_t>( raw_window ) );
            }

            return windows;
        }

        [[nodiscard]]
        grab::Result<std::optional<Geometry>>
        read_geometry( xcb_connection_t* connection,
                       xcb_window_t      root,
                       xcb_window_t      window )
        {
            xcb_generic_error_t* raw_geometry_error = nullptr;
            const auto           geometry           = take_xcb_owned(
                xcb_get_geometry_reply( connection,
                                        xcb_get_geometry( connection, window ),
                                        &raw_geometry_error )
            );
            const auto geometry_error = take_xcb_owned( raw_geometry_error );
            if( geometry_error != nullptr )
            {
                if( stale_window_error( *geometry_error ) )
                {
                    return std::optional<Geometry>{};
                }
                return grab::fail( grab::ErrorCode::ProtocolError,
                                   "XCB GetGeometry failed" );
            }
            if( geometry == nullptr )
            {
                return std::optional<Geometry>{};
            }

            Geometry result{
                .x      = geometry->x,
                .y      = geometry->y,
                .width  = geometry->width,
                .height = geometry->height,
            };

            xcb_generic_error_t* raw_translate_error = nullptr;
            const auto           translation         = take_xcb_owned(
                xcb_translate_coordinates_reply( connection,
                                                 xcb_translate_coordinates( connection,
                                                                            window,
                                                                            root,
                                                                            rootOrigin,
                                                                            rootOrigin ),
                                                 &raw_translate_error )
            );
            const auto translate_error = take_xcb_owned( raw_translate_error );
            if( translate_error != nullptr )
            {
                if( stale_window_error( *translate_error ) )
                {
                    return std::optional<Geometry>{};
                }
                return grab::fail( grab::ErrorCode::ProtocolError,
                                   "XCB TranslateCoordinates failed" );
            }
            if( translation != nullptr )
            {
                result.x = translation->dst_x;
                result.y = translation->dst_y;
            }

            return result;
        }

        [[nodiscard]]
        bool
        randr_is_present( xcb_connection_t* connection )
        {
            xcb_generic_error_t* raw_error = nullptr;
            const auto           reply     = take_xcb_owned( xcb_query_extension_reply(
                connection,
                xcb_query_extension(
                    connection,
                    static_cast<std::uint16_t>( randrExtensionName.size() ),
                    randrExtensionName.data()
                ),
                &raw_error
            ) );
            const auto           error     = take_xcb_owned( raw_error );
            return error == nullptr && reply != nullptr && reply->present != 0U;
        }

        [[nodiscard]]
        OutputInfo
        fallback_output( const ScreenInfo& screen )
        {
            const Geometry bounds{
                .x      = 0,
                .y      = 0,
                .width  = screen.width,
                .height = screen.height,
            };
            return OutputInfo{
                .name   = std::string{ fallbackOutputName },
                .bounds = bounds,
            };
        }

        [[nodiscard]]
        std::string
        output_name( const xcb_randr_get_output_info_reply_t& output_info,
                     xcb_randr_output_t                       output )
        {
            const int name_length =
                xcb_randr_get_output_info_name_length( &output_info );
            if( name_length <= 0 )
            {
                return std::string{ generatedOutputPrefix } +
                       std::to_string( static_cast<std::uint32_t>( output ) );
            }

            const auto* const name_bytes =
                xcb_randr_get_output_info_name( &output_info );
            std::string name;
            name.reserve( static_cast<std::size_t>( name_length ) );
            for( const auto value : std::span<const std::uint8_t>{
                     name_bytes,
                     static_cast<std::size_t>( name_length )
                 } )
            {
                name.push_back( static_cast<char>( value ) );
            }
            return name;
        }

        [[nodiscard]]
        grab::Result<std::vector<OutputInfo>>
        randr_outputs( xcb_connection_t* connection,
                       const ScreenInfo& screen )
        {
            if( !randr_is_present( connection ) )
            {
                return std::vector<OutputInfo>{ fallback_output( screen ) };
            }

            xcb_generic_error_t* raw_resources_error = nullptr;
            const auto           resources =
                take_xcb_owned( xcb_randr_get_screen_resources_current_reply(
                    connection,
                    xcb_randr_get_screen_resources_current( connection, screen.root ),
                    &raw_resources_error
                ) );
            const auto resources_error = take_xcb_owned( raw_resources_error );
            if( resources_error != nullptr || resources == nullptr )
            {
                return grab::fail( grab::ErrorCode::ProtocolError,
                                   "RandR screen resources query failed" );
            }

            const int output_count =
                xcb_randr_get_screen_resources_current_outputs_length( resources.get() );
            if( output_count <= 0 )
            {
                return std::vector<OutputInfo>{ fallback_output( screen ) };
            }

            const std::span<const xcb_randr_output_t> outputs{
                xcb_randr_get_screen_resources_current_outputs( resources.get() ),
                static_cast<std::size_t>( output_count ),
            };

            std::vector<OutputInfo> result;
            result.reserve( outputs.size() );
            for( const xcb_randr_output_t output : outputs )
            {
                xcb_generic_error_t* raw_output_error = nullptr;
                const auto output_info = take_xcb_owned( xcb_randr_get_output_info_reply(
                    connection,
                    xcb_randr_get_output_info( connection,
                                               output,
                                               resources->config_timestamp ),
                    &raw_output_error
                ) );
                const auto output_error = take_xcb_owned( raw_output_error );
                if( output_error != nullptr || output_info == nullptr )
                {
                    return grab::fail( grab::ErrorCode::ProtocolError,
                                       "RandR output info query failed" );
                }
                if( output_info->connection !=
                    XCB_RANDR_CONNECTION_CONNECTED ||
                    output_info->crtc == XCB_NONE )
                {
                    continue;
                }

                xcb_generic_error_t* raw_crtc_error = nullptr;
                const auto crtc_info  = take_xcb_owned( xcb_randr_get_crtc_info_reply(
                    connection,
                    xcb_randr_get_crtc_info( connection,
                                             output_info->crtc,
                                             resources->config_timestamp ),
                    &raw_crtc_error
                ) );
                const auto crtc_error = take_xcb_owned( raw_crtc_error );
                if( crtc_error != nullptr || crtc_info == nullptr )
                {
                    return grab::fail( grab::ErrorCode::ProtocolError,
                                       "RandR CRTC info query failed" );
                }

                const Geometry bounds{
                    .x      = crtc_info->x,
                    .y      = crtc_info->y,
                    .width  = crtc_info->width,
                    .height = crtc_info->height,
                };
                result.push_back( OutputInfo{
                    .name   = output_name( *output_info, output ),
                    .bounds = bounds,
                } );
            }

            if( result.empty() )
            {
                result.push_back( fallback_output( screen ) );
            }
            return result;
        }

    }    // namespace

    grab::Result<std::vector<WindowInfo>>
    list_windows( const char* display )
    {
        auto connected = connect_display( display );
        if( !connected.has_value() )
        {
            return std::unexpected( std::move( connected.error() ) );
        }

        auto atoms = intern_atoms( connected->connection.get() );
        if( !atoms.has_value() )
        {
            return std::unexpected( std::move( atoms.error() ) );
        }

        auto windows = client_list_windows( connected->connection.get(),
                                            connected->screen.root,
                                            atoms->net_client_list );
        if( !windows.has_value() )
        {
            return std::unexpected( std::move( windows.error() ) );
        }

        std::vector<WindowInfo> result;
        result.reserve( windows->size() );
        for( const xcb_window_t window : *windows )
        {
            auto geometry = read_geometry( connected->connection.get(),
                                           connected->screen.root,
                                           window );
            if( !geometry.has_value() )
            {
                return std::unexpected( std::move( geometry.error() ) );
            }
            if( !geometry->has_value() )
            {
                continue;
            }

            auto wm_class = read_wm_class( connected->connection.get(), window );
            if( !wm_class.has_value() )
            {
                return std::unexpected( std::move( wm_class.error() ) );
            }

            auto title = read_title( connected->connection.get(), window, *atoms );
            if( !title.has_value() )
            {
                return std::unexpected( std::move( title.error() ) );
            }

            result.push_back( WindowInfo{
                .id       = static_cast<std::uint32_t>( window ),
                .wm_class = std::move( *wm_class ),
                .title    = std::move( *title ),
                .bounds   = **geometry,
            } );
        }

        return result;
    }

    grab::Result<std::vector<OutputInfo>>
    list_outputs( const char* display )
    {
        auto connected = connect_display( display );
        if( !connected.has_value() )
        {
            return std::unexpected( std::move( connected.error() ) );
        }

        return randr_outputs( connected->connection.get(), connected->screen );
    }

}    // namespace grab::screen
