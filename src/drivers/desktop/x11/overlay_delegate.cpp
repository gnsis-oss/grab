#include "drivers/desktop/x11/overlay_delegate.hpp"
#include "drivers/desktop/x11/present_policy.hpp"
#include "grab/capability.hpp"
#include "grab/geometry/rectangle.hpp"
#include "grab/geometry/size.hpp"
#include "grab/image.hpp"
#include "grab/overlay.hpp"
#include "grab/result.hpp"
#include "grab/space.hpp"
#include "kernel/presentation/overlay_animation.hpp"
#include "kernel/presentation/overlay_raster.hpp"
#include "kernel/scheduling/pacing_governor.hpp"
#include "kernel/scheduling/reactor.hpp"
#include "kernel/support/diag.hpp"
#include "kernel/support/log.hpp"
#include "kernel/support/log_tags.hpp"
#include "spi/overlay_delegate.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
// clang-format off
#include <sys/epoll.h>    // NOLINT(misc-include-cleaner)
#include <sys/ipc.h>
#include <sys/poll.h>
#include <sys/shm.h>
// clang-format on
#include <tuple>
#include <utility>
#include <variant>
#include <vector>
#include <xcb/randr.h>
#include <xcb/render.h>
#include <xcb/shape.h>
#include <xcb/shm.h>
#include <xcb/xcb.h>
#include <xcb/xcbext.h>
#include <xcb/xfixes.h>
#include <xcb/xproto.h>

namespace grab::drivers::desktop::x11
{
    namespace
    {

        constexpr std::uint32_t targetFramesPerSecond = 60U;
        constexpr std::uint8_t  argbDepth             = 32U;
        constexpr std::uint8_t  bitsPerByte           = 8U;
        constexpr std::uint8_t  bgraBytesPerPixel     = 4U;
        constexpr std::uint8_t  fullChannel           = 0XFFU;
        constexpr std::uint8_t  responseTypeMask      = 0X7FU;
        constexpr std::uint8_t  noEventPropagation    = 0U;
        constexpr std::uint8_t  doNotUseOwnerEvents   = 0U;
        constexpr std::uint8_t  noShmCompletionEvent  = 0U;
        constexpr std::uint32_t revisionStep          = 1U;
        constexpr int           xcbSuccess            = 0;
        constexpr int           flushFailure          = 0;
        constexpr int           invalidFileDescriptor = -1;
        constexpr int           systemCallFailure     = -1;
        constexpr std::uint32_t generatedIdFailure =
            std::numeric_limits<std::uint32_t>::max();
        constexpr int           noPollEvents      = 0;
        constexpr std::int16_t  xcbPollEvents     = POLLIN;
        constexpr int           shmPermissions    = 0600;
        constexpr auto          noTimerDelay      = std::chrono::nanoseconds::zero();
        constexpr auto          flushFenceTimeout = std::chrono::seconds{ 2 };

        constexpr std::uint32_t passiveWindowEventMask = XCB_EVENT_MASK_EXPOSURE;
        constexpr std::uint32_t editWindowEventMask    = passiveWindowEventMask |
                                                         XCB_EVENT_MASK_BUTTON_PRESS |
                                                         XCB_EVENT_MASK_BUTTON_RELEASE |
                                                         XCB_EVENT_MASK_POINTER_MOTION |
                                                         XCB_EVENT_MASK_ENTER_WINDOW |
                                                         XCB_EVENT_MASK_LEAVE_WINDOW;
        constexpr std::uint16_t pointerGrabEventMask =
            XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_BUTTON_MOTION;
        constexpr std::int64_t nativeCoordinateExtent =
            static_cast<std::int64_t>( std::numeric_limits<std::int16_t>::max() ) + 1;
        constexpr std::uint32_t    minimumXfixesRegionMajorVersion = 2U;
        constexpr std::uint16_t    minimumShapeInputMajorVersion   = 1U;
        constexpr std::uint16_t    minimumShapeInputMinorVersion   = 1U;

        constexpr std::uint8_t     bgraBlueByteIndex               = 0U;
        constexpr std::uint8_t     bgraGreenByteIndex              = 1U;
        constexpr std::uint8_t     bgraRedByteIndex                = 2U;
        constexpr std::uint8_t     bgraAlphaByteIndex              = 3U;
        constexpr std::uint8_t     bgraBitsPerPixel = bgraBytesPerPixel * bitsPerByte;
        constexpr std::string_view argbUnavailableReason{
            "X11 overlay requires an XRender ARGB32 visual"
        };
        constexpr std::string_view xfixesUnavailableReason{
            "X11 overlay requires XFixes ShapeInput support"
        };
        constexpr std::string_view compositorUnavailableReason{
            "X11 overlay requires an owned compositing manager selection"
        };
        constexpr std::string_view reactorRequiredReason{
            "X11 overlay requires a bound reactor to map (compositor monitor "
            "and lifetime/animation frame clock)"
        };
        constexpr std::string_view compositorSelectionPrefix{ "_NET_WM_CM_S" };
        constexpr bool             hostByteOrderKnown{
            std::endian::native ==
            std::endian::little ||
            std::endian::native == std::endian::big
        };
        constexpr std::uint8_t hostImageByteOrder =
            std::endian::native == std::endian::little ? XCB_IMAGE_ORDER_LSB_FIRST
                                                       : XCB_IMAGE_ORDER_MSB_FIRST;

        [[nodiscard]]
        constexpr std::uint32_t
        native_bgra_channel_mask( std::uint8_t byte_index ) noexcept
        {
            const auto native_byte_index =
                std::endian::native == std::endian::little
                    ? byte_index
                    : static_cast<std::uint8_t>( bgraAlphaByteIndex - byte_index );
            return static_cast<std::uint32_t>( fullChannel )
                << ( static_cast<std::uint32_t>( native_byte_index ) * bitsPerByte );
        }

        constexpr std::uint32_t nativeBgraBlueMask =
            native_bgra_channel_mask( bgraBlueByteIndex );
        constexpr std::uint32_t nativeBgraGreenMask =
            native_bgra_channel_mask( bgraGreenByteIndex );
        constexpr std::uint32_t nativeBgraRedMask =
            native_bgra_channel_mask( bgraRedByteIndex );
        constexpr std::uint32_t nativeBgraAlphaMask =
            native_bgra_channel_mask( bgraAlphaByteIndex );

        template<typename Type>
        using XcbOwned = std::unique_ptr<Type, decltype( &std::free )>;

        template<typename Type>
        [[nodiscard]]
        XcbOwned<Type>
        take_xcb_owned( Type* pointer ) noexcept
        {
            return XcbOwned<Type>{ pointer, &std::free };
        }

        template<typename Event>
        [[nodiscard]]
        const Event*
        event_as( const xcb_generic_event_t* event ) noexcept
        {
            // XCB exposes all wire events through its generic event prefix.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            return reinterpret_cast<const Event*>( event );
        }

        struct ConnectionDeleter
        {
                void
                operator()( xcb_connection_t* connection ) const noexcept
                {
                    if( connection != nullptr )
                    {
                        xcb_disconnect( connection );
                    }
                }
        };

        using OwnedConnection = std::unique_ptr<xcb_connection_t, ConnectionDeleter>;

        struct ScreenSpec
        {
                xcb_window_t   root{};
                xcb_visualid_t root_visual{};
                std::uint16_t  width{};
                std::uint16_t  height{};
                std::uint8_t   image_byte_order{};
                int            screen_index{};
        };

        struct NativeLayout
        {
                xcb_visualid_t visual{};
                std::uint8_t   depth{};
                std::uint8_t   bits_per_pixel{};
                std::uint8_t   scanline_pad{};
                std::uint8_t   image_byte_order{};
                std::uint32_t  red_mask{};
                std::uint32_t  green_mask{};
                std::uint32_t  blue_mask{};
                std::uint32_t  alpha_mask{};
        };

        [[nodiscard]]
        constexpr bool
        bgra_row_strides_compatible( std::uint16_t width,
                                     std::size_t   source_stride,
                                     std::size_t   destination_stride ) noexcept
        {
            const auto row_bytes = static_cast<std::size_t>( width ) * bgraBytesPerPixel;
            return source_stride >= row_bytes && destination_stride >= row_bytes;
        }

        [[nodiscard]]
        constexpr bool
        native_bgra_memcpy_compatible( const NativeLayout& layout,
                                       std::uint16_t       width,
                                       std::size_t         source_stride,
                                       std::size_t         destination_stride ) noexcept
        {
            const bool masks_match = layout.red_mask ==
                                     nativeBgraRedMask &&
                                     layout.green_mask ==
                                     nativeBgraGreenMask &&
                                     layout.blue_mask ==
                                     nativeBgraBlueMask &&
                                     layout.alpha_mask == nativeBgraAlphaMask;
            return hostByteOrderKnown &&
                   layout.bits_per_pixel ==
                   bgraBitsPerPixel &&
                   masks_match &&
                   layout.image_byte_order ==
                   hostImageByteOrder &&
                   bgra_row_strides_compatible( width,
                                                source_stride,
                                                destination_stride );
        }

        struct ExtensionSpec
        {
                std::uint8_t xfixes_first_event{};
                std::uint8_t randr_first_event{};
                bool         randr_available{};
                bool         shm_available{};
        };

        struct ProbeData
        {
                ScreenSpec    screen;
                NativeLayout  layout;
                ExtensionSpec extensions;
                xcb_atom_t    compositor_selection{};
                xcb_window_t  compositor_owner{};
                // Present the whole surface on damaged frames — see
                // present_policy.hpp for why a PRIME-sink display needs it.
                bool          full_present{};
        };

        struct ConnectedDisplay
        {
                OwnedConnection connection;
                int             screen_index{};
        };

        [[nodiscard]]
        Error
        make_error( ErrorCode   code,
                    std::string message )
        {
            return Error{
                .code       = code,
                .message    = std::move( message ),
                .capability = {},
                .target     = "x11",
                .attempts   = {},
            };
        }

        [[nodiscard]]
        Error
        capability_error( std::string_view reason )
        {
            const std::string detail{ reason };
            return Error{
                .code       = ErrorCode::CapabilityUnavailable,
                .message    = detail,
                .capability = std::string{ capability::overlay },
                .target     = "x11",
                .attempts   = {
                                          ProviderAttempt{
                        .provider = "x11",
                        .reason   = detail,
                    }, },
            };
        }

        [[nodiscard]]
        std::string
        system_error_message( std::string_view operation )
        {
            return std::string{ operation } +
                   " failed: " +
                   std::error_code{ errno, std::generic_category() }.message();
        }

        [[nodiscard]]
        Result<void>
        check_request( xcb_connection_t* connection,
                       xcb_void_cookie_t cookie,
                       std::string_view  operation )
        {
            const auto error = take_xcb_owned( xcb_request_check( connection, cookie ) );
            if( error == nullptr )
            {
                return {};
            }
            return std::unexpected(
                make_error( ErrorCode::ProtocolError,
                            std::string{ operation } +
                                " failed with X error " +
                                std::to_string( error->error_code ) )
            );
        }

        [[nodiscard]]
        Result<ConnectedDisplay>
        connect_display( std::string_view display )
        {
            const std::string display_storage{ display };
            int               screen_index{};
            OwnedConnection   connection{
                xcb_connect( display_storage.empty() ? nullptr : display_storage.c_str(),
                             &screen_index )
            };
            if( connection ==
                nullptr ||
                xcb_connection_has_error( connection.get() ) != xcbSuccess )
            {
                return std::unexpected(
                    make_error( ErrorCode::DisplayUnavailable,
                                "unable to open X display for overlay" )
                );
            }
            return ConnectedDisplay{
                .connection   = std::move( connection ),
                .screen_index = screen_index,
            };
        }

        [[nodiscard]]
        Result<ScreenSpec>
        screen_spec( xcb_connection_t* connection,
                     int               screen_index )
        {
            const xcb_setup_t* const setup = xcb_get_setup( connection );
            if( setup == nullptr )
            {
                return std::unexpected(
                    make_error( ErrorCode::DisplayUnavailable,
                                "X11 overlay display setup is unavailable" )
                );
            }

            auto screens = xcb_setup_roots_iterator( setup );
            for( int current{}; current < screen_index && screens.rem > 0; ++current )
            {
                xcb_screen_next( &screens );
            }
            if( screens.data == nullptr )
            {
                return std::unexpected(
                    make_error( ErrorCode::DisplayUnavailable,
                                "X11 overlay screen is unavailable" )
                );
            }

            return ScreenSpec{
                .root             = screens.data->root,
                .root_visual      = screens.data->root_visual,
                .width            = screens.data->width_in_pixels,
                .height           = screens.data->height_in_pixels,
                .image_byte_order = setup->image_byte_order,
                .screen_index     = screen_index,
            };
        }

        [[nodiscard]]
        bool
        extension_present( xcb_connection_t* connection,
                           xcb_extension_t*  extension )
        {
            const auto* const data = xcb_get_extension_data( connection, extension );
            return data != nullptr && data->present != 0U;
        }

        [[nodiscard]]
        const xcb_render_pictforminfo_t*
        argb_format( const xcb_render_query_pict_formats_reply_t& formats,
                     xcb_render_pictformat_t                      id )
        {
            auto iterator = xcb_render_query_pict_formats_formats_iterator( &formats );
            while( iterator.rem > 0 )
            {
                const auto& format = *iterator.data;
                if( format.id ==
                    id &&
                    format.type ==
                    XCB_RENDER_PICT_TYPE_DIRECT &&
                    format.depth ==
                    argbDepth &&
                    format.direct.red_mask !=
                    0U &&
                    format.direct.green_mask !=
                    0U &&
                    format.direct.blue_mask !=
                    0U &&
                    format.direct.alpha_mask != 0U )
                {
                    return iterator.data;
                }
                xcb_render_pictforminfo_next( &iterator );
            }
            return nullptr;
        }

        [[nodiscard]]
        const xcb_visualtype_t*
        visual_type( xcb_connection_t* connection,
                     xcb_visualid_t    visual )
        {
            const auto* const setup = xcb_get_setup( connection );
            if( setup == nullptr )
            {
                return nullptr;
            }
            auto screens = xcb_setup_roots_iterator( setup );
            while( screens.rem > 0 )
            {
                auto depths = xcb_screen_allowed_depths_iterator( screens.data );
                while( depths.rem > 0 )
                {
                    auto visuals = xcb_depth_visuals_iterator( depths.data );
                    while( visuals.rem > 0 )
                    {
                        if( visuals.data->visual_id == visual )
                        {
                            return visuals.data;
                        }
                        xcb_visualtype_next( &visuals );
                    }
                    xcb_depth_next( &depths );
                }
                xcb_screen_next( &screens );
            }
            return nullptr;
        }

        [[nodiscard]]
        std::optional<std::pair<std::uint8_t,
                                std::uint8_t>>
        pixmap_layout( xcb_connection_t* connection,
                       std::uint8_t      depth )
        {
            const auto* const setup = xcb_get_setup( connection );
            if( setup == nullptr )
            {
                return std::nullopt;
            }
            auto formats = xcb_setup_pixmap_formats_iterator( setup );
            while( formats.rem > 0 )
            {
                if( formats.data->depth == depth )
                {
                    return std::pair{
                        formats.data->bits_per_pixel,
                        formats.data->scanline_pad
                    };
                }
                xcb_format_next( &formats );
            }
            return std::nullopt;
        }

        [[nodiscard]]
        Result<NativeLayout>
        find_argb_layout( xcb_connection_t* connection,
                          const ScreenSpec& screen )
        {
            if( !extension_present( connection, &xcb_render_id ) )
            {
                return std::unexpected( capability_error( argbUnavailableReason ) );
            }

            xcb_generic_error_t* raw_error{};
            const auto formats = take_xcb_owned( xcb_render_query_pict_formats_reply(
                connection,
                xcb_render_query_pict_formats( connection ),
                &raw_error
            ) );
            const auto error   = take_xcb_owned( raw_error );
            if( error != nullptr || formats == nullptr )
            {
                return std::unexpected( capability_error( argbUnavailableReason ) );
            }

            auto screens =
                xcb_render_query_pict_formats_screens_iterator( formats.get() );
            for( int current{}; current < screen.screen_index && screens.rem > 0;
                 ++current )
            {
                xcb_render_pictscreen_next( &screens );
            }
            if( screens.data == nullptr )
            {
                return std::unexpected( capability_error( argbUnavailableReason ) );
            }

            auto depths = xcb_render_pictscreen_depths_iterator( screens.data );
            while( depths.rem > 0 )
            {
                if( depths.data->depth == argbDepth )
                {
                    auto visuals = xcb_render_pictdepth_visuals_iterator( depths.data );
                    while( visuals.rem > 0 )
                    {
                        const auto* const format =
                            argb_format( *formats, visuals.data->format );
                        const auto* const native_visual =
                            visual_type( connection, visuals.data->visual );
                        const auto pixmap = pixmap_layout( connection, argbDepth );
                        if( format != nullptr && native_visual != nullptr && pixmap )
                        {
                            const auto shifted_mask =
                                []( std::uint16_t mask, std::uint16_t shift )
                            {
                                return static_cast<std::uint32_t>( mask ) << shift;
                            };
                            return NativeLayout{
                                .visual           = visuals.data->visual,
                                .depth            = argbDepth,
                                .bits_per_pixel   = pixmap->first,
                                .scanline_pad     = pixmap->second,
                                .image_byte_order = screen.image_byte_order,
                                .red_mask         = native_visual->red_mask,
                                .green_mask       = native_visual->green_mask,
                                .blue_mask        = native_visual->blue_mask,
                                .alpha_mask = shifted_mask( format->direct.alpha_mask,
                                                            format->direct.alpha_shift ),
                            };
                        }
                        xcb_render_pictvisual_next( &visuals );
                    }
                }
                xcb_render_pictdepth_next( &depths );
            }
            return std::unexpected( capability_error( argbUnavailableReason ) );
        }

        [[nodiscard]]
        Result<std::uint8_t>
        negotiate_xfixes_shape_input( xcb_connection_t* connection )
        {
            if( connection == nullptr )
            {
                return fail( ErrorCode::InvalidArgument,
                             "X11 overlay XFixes negotiation requires a connection" );
            }

            const auto* const xfixes =
                xcb_get_extension_data( connection, &xcb_xfixes_id );
            const bool xfixes_present = xfixes != nullptr && xfixes->present != 0U;
            if( !xfixes_present || !extension_present( connection, &xcb_shape_id ) )
            {
                return std::unexpected( capability_error( xfixesUnavailableReason ) );
            }

            xcb_generic_error_t* raw_xfixes_error{};
            const auto xfixes_version = take_xcb_owned( xcb_xfixes_query_version_reply(
                connection,
                xcb_xfixes_query_version( connection,
                                          XCB_XFIXES_MAJOR_VERSION,
                                          XCB_XFIXES_MINOR_VERSION ),
                &raw_xfixes_error
            ) );
            const auto xfixes_error   = take_xcb_owned( raw_xfixes_error );
            xcb_generic_error_t* raw_shape_error{};
            const auto           shape_version = take_xcb_owned(
                xcb_shape_query_version_reply( connection,
                                               xcb_shape_query_version( connection ),
                                               &raw_shape_error )
            );
            const auto shape_error = take_xcb_owned( raw_shape_error );
            if( xfixes_error !=
                nullptr ||
                xfixes_version ==
                nullptr ||
                shape_error !=
                nullptr ||
                shape_version == nullptr )
            {
                return std::unexpected( capability_error( xfixesUnavailableReason ) );
            }

            const bool xfixes_regions_supported =
                xfixes_version->major_version >= minimumXfixesRegionMajorVersion;
            const bool shape_input_supported =
                shape_version->major_version >
                minimumShapeInputMajorVersion ||
                ( shape_version->major_version ==
                  minimumShapeInputMajorVersion &&
                  shape_version->minor_version >= minimumShapeInputMinorVersion );
            if( !xfixes_regions_supported || !shape_input_supported )
            {
                return std::unexpected( capability_error( xfixesUnavailableReason ) );
            }

            return xfixes->first_event;
        }

        [[nodiscard]]
        Result<ExtensionSpec>
        probe_extensions( xcb_connection_t* connection )
        {
            auto xfixes_first_event = negotiate_xfixes_shape_input( connection );
            if( !xfixes_first_event.has_value() )
            {
                return std::unexpected( std::move( xfixes_first_event.error() ) );
            }

            ExtensionSpec result{
                .xfixes_first_event = *xfixes_first_event,
            };
            const auto* const randr =
                xcb_get_extension_data( connection, &xcb_randr_id );
            if( randr != nullptr && randr->present != 0U )
            {
                result.randr_first_event = randr->first_event;
                result.randr_available   = true;
            }
            if( extension_present( connection, &xcb_shm_id ) )
            {
                xcb_generic_error_t* raw_shm_error{};
                const auto           shm_version = take_xcb_owned(
                    xcb_shm_query_version_reply( connection,
                                                 xcb_shm_query_version( connection ),
                                                 &raw_shm_error )
                );
                const auto shm_error = take_xcb_owned( raw_shm_error );
                result.shm_available = shm_error == nullptr && shm_version != nullptr;
            }
            return result;
        }

        [[nodiscard]]
        Result<xcb_atom_t>
        compositor_atom( xcb_connection_t* connection,
                         int               screen_index )
        {
            const std::string    name = std::string{ compositorSelectionPrefix } +
                                        std::to_string( screen_index );
            xcb_generic_error_t* raw_error{};
            const auto           reply = take_xcb_owned( xcb_intern_atom_reply(
                connection,
                xcb_intern_atom( connection,
                                 0U,
                                 static_cast<std::uint16_t>( name.size() ),
                                 name.data() ),
                &raw_error
            ) );
            const auto           error = take_xcb_owned( raw_error );
            if( error != nullptr || reply == nullptr )
            {
                return std::unexpected(
                    make_error( ErrorCode::ProtocolError,
                                "X11 overlay compositor selection lookup failed" )
                );
            }
            return reply->atom;
        }

        [[nodiscard]]
        Result<xcb_window_t>
        selection_owner( xcb_connection_t* connection,
                         xcb_atom_t        selection )
        {
            xcb_generic_error_t* raw_error{};
            const auto           reply = take_xcb_owned( xcb_get_selection_owner_reply(
                connection,
                xcb_get_selection_owner( connection, selection ),
                &raw_error
            ) );
            const auto           error = take_xcb_owned( raw_error );
            if( error != nullptr || reply == nullptr )
            {
                return std::unexpected(
                    make_error( ErrorCode::ProtocolError,
                                "X11 overlay compositor ownership query failed" )
                );
            }
            return reply->owner;
        }

        // A reverse-PRIME topology: one provider renders (SourceOutput),
        // another scans out a COPY (SinkOutput). The copy is refreshed by
        // dirty-region tracking, which is what makes incremental presents
        // unreliable on the glass — see present_policy.hpp. Any failure to
        // answer reads as "no sink": an old server without RandR 1.4 cannot
        // have one.
        [[nodiscard]]
        bool
        prime_sink_present( xcb_connection_t* connection,
                            xcb_window_t      root,
                            bool              randr_available )
        {
            if( !randr_available )
            {
                return false;
            }
            xcb_generic_error_t* raw_error{};
            const auto           reply = take_xcb_owned( xcb_randr_get_providers_reply(
                connection,
                xcb_randr_get_providers( connection, root ),
                &raw_error
            ) );
            const auto           error = take_xcb_owned( raw_error );
            if( error != nullptr || reply == nullptr )
            {
                return false;
            }
            const auto providers = std::span{
                xcb_randr_get_providers_providers( reply.get() ),
                static_cast<std::size_t>(
                    std::max( 0,
                              xcb_randr_get_providers_providers_length( reply.get() ) )
                )
            };
            bool source_seen = false;
            bool sink_seen   = false;
            for( const xcb_randr_provider_t provider : providers )
            {
                xcb_generic_error_t* raw_info_error{};
                const auto info = take_xcb_owned( xcb_randr_get_provider_info_reply(
                    connection,
                    xcb_randr_get_provider_info( connection,
                                                 provider,
                                                 reply->timestamp ),
                    &raw_info_error
                ) );
                const auto info_error = take_xcb_owned( raw_info_error );
                if( info_error != nullptr || info == nullptr )
                {
                    continue;
                }
                source_seen = source_seen ||
                              ( info->capabilities &
                                XCB_RANDR_PROVIDER_CAPABILITY_SOURCE_OUTPUT ) != 0U;
                sink_seen   = sink_seen ||
                              ( info->capabilities &
                                XCB_RANDR_PROVIDER_CAPABILITY_SINK_OUTPUT ) != 0U;
            }
            return providers.size() >= 2U && source_seen && sink_seen;
        }

        [[nodiscard]]
        PresentPolicy
        present_policy_from_env()
        {
            // NOLINTNEXTLINE(concurrency-mt-unsafe)
            const char* const value = std::getenv( "GRAB_OVERLAY_PRESENT" );
            return parse_present_policy( value == nullptr ? std::string_view{}
                                                          : std::string_view{ value } );
        }

        [[nodiscard]]
        Result<ProbeData>
        probe_connection( xcb_connection_t* connection,
                          int               screen_index,
                          bool              require_compositor )
        {
            auto screen = screen_spec( connection, screen_index );
            if( !screen.has_value() )
            {
                return std::unexpected( std::move( screen.error() ) );
            }
            auto layout = find_argb_layout( connection, *screen );
            if( !layout.has_value() )
            {
                return std::unexpected( std::move( layout.error() ) );
            }
            auto extensions = probe_extensions( connection );
            if( !extensions.has_value() )
            {
                return std::unexpected( std::move( extensions.error() ) );
            }
            auto atom = compositor_atom( connection, screen_index );
            if( !atom.has_value() )
            {
                return std::unexpected( std::move( atom.error() ) );
            }
            auto owner = selection_owner( connection, *atom );
            if( !owner.has_value() )
            {
                return std::unexpected( std::move( owner.error() ) );
            }
            if( require_compositor && *owner == XCB_WINDOW_NONE )
            {
                return std::unexpected(
                    capability_error( compositorUnavailableReason )
                );
            }
            const bool sink = prime_sink_present( connection,
                                                  screen->root,
                                                  extensions->randr_available );
            const bool full = full_present_selected( present_policy_from_env(), sink );
            log::nominal(
                [&]( auto& event )
                {
                    event.tag( log::tags::present )
                        .value( "policy", full ? "full" : "incremental" )
                        .value( "prime_sink", sink );
                }
            );
            return ProbeData{
                .screen               = *screen,
                .layout               = *layout,
                .extensions           = *extensions,
                .compositor_selection = *atom,
                .compositor_owner     = *owner,
                .full_present         = full,
            };
        }

        [[nodiscard]]
        std::chrono::milliseconds
        monotonic_milliseconds() noexcept
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            );
        }

        [[nodiscard]]
        std::chrono::milliseconds
        saturating_deadline( std::chrono::milliseconds start,
                             std::chrono::milliseconds duration ) noexcept
        {
            using Rep              = std::chrono::milliseconds::rep;
            constexpr auto maximum = std::numeric_limits<Rep>::max();
            constexpr auto minimum = std::numeric_limits<Rep>::min();
            const auto     left    = start.count();
            const auto     right   = duration.count();
            if( right > Rep{} && left > maximum - right )
            {
                return std::chrono::milliseconds{ maximum };
            }
            if( right < Rep{} && left < minimum - right )
            {
                return std::chrono::milliseconds{ minimum };
            }
            return std::chrono::milliseconds{ left + right };
        }

        [[nodiscard]]
        std::optional<std::chrono::milliseconds>
        shape_deadline( const overlay::ShapeRecord& record ) noexcept
        {
            if( const auto* ttl = std::get_if<overlay::Ttl>( &record.shape.lifetime ) )
            {
                return saturating_deadline( record.started_at, ttl->duration );
            }
            if( const auto* fade = std::get_if<overlay::Fade>( &record.shape.lifetime ) )
            {
                return saturating_deadline( record.started_at, fade->duration );
            }
            return std::nullopt;
        }

        [[nodiscard]]
        std::optional<std::chrono::milliseconds>
        animation_deadline( const overlay::ShapeRecord& record ) noexcept
        {
            if( !record.shape.animation.has_value() )
            {
                return std::nullopt;
            }
            return saturating_deadline(
                record.started_at,
                kernel::presentation::animation_duration( *record.shape.animation )
            );
        }

        [[nodiscard]]
        Result<void>
        apply_negotiated_xfixes_input_region(
            xcb_connection_t*                connection,
            xcb_window_t                     window,
            std::span<const xcb_rectangle_t> rectangles
        )
        {
            if( connection == nullptr || window == XCB_WINDOW_NONE )
            {
                return fail( ErrorCode::InvalidArgument,
                             "X11 overlay ShapeInput region requires a window" );
            }

            const auto region = xcb_generate_id( connection );
            if( region == generatedIdFailure )
            {
                return fail( ErrorCode::ProtocolError,
                             "allocate X11 overlay XFixes region failed" );
            }

            auto created =
                check_request( connection,
                               xcb_xfixes_create_region_checked(
                                   connection,
                                   region,
                                   static_cast<std::uint32_t>( rectangles.size() ),
                                   rectangles.empty() ? nullptr : rectangles.data()
                               ),
                               "create X11 overlay XFixes input region" );
            if( !created.has_value() )
            {
                return created;
            }

            auto installed = check_request(
                connection,
                xcb_xfixes_set_window_shape_region_checked( connection,
                                                            window,
                                                            XCB_SHAPE_SK_INPUT,
                                                            0,
                                                            0,
                                                            region ),
                "apply X11 overlay XFixes ShapeInput region"
            );
            auto destroyed =
                check_request( connection,
                               xcb_xfixes_destroy_region_checked( connection, region ),
                               "destroy temporary X11 overlay XFixes input region" );
            if( !installed.has_value() )
            {
                return installed;
            }

            // SetWindowShapeRegion copies the region. Cleanup failure cannot roll the
            // successfully installed window shape back, so keep the applied result.
            static_cast<void>( destroyed );
            return {};
        }

        [[nodiscard]]
        Result<void>
        apply_xfixes_input_region( xcb_connection_t*                connection,
                                   xcb_window_t                     window,
                                   std::span<const xcb_rectangle_t> rectangles )
        {
            if( connection == nullptr || window == XCB_WINDOW_NONE )
            {
                return apply_negotiated_xfixes_input_region( connection,
                                                             window,
                                                             rectangles );
            }

            auto negotiated = negotiate_xfixes_shape_input( connection );
            if( !negotiated.has_value() )
            {
                return std::unexpected( std::move( negotiated.error() ) );
            }
            return apply_negotiated_xfixes_input_region( connection,
                                                         window,
                                                         rectangles );
        }

    }    // namespace

    namespace detail
    {

        std::optional<std::string_view>
        overlay_probe_reason( OverlayProbePrerequisites prerequisites ) noexcept
        {
            if( !prerequisites.argb32_visual )
            {
                return argbUnavailableReason;
            }
            if( !prerequisites.xfixes_shape_input )
            {
                return xfixesUnavailableReason;
            }
            if( !prerequisites.compositor_owner )
            {
                return compositorUnavailableReason;
            }
            return std::nullopt;
        }

        OverlayDamagePlan
        overlay_damage_plan( std::span<const overlay::ShapeRecord> shapes,
                             std::chrono::milliseconds             now,
                             bool                                  scene_dirty ) noexcept
        {
            OverlayDamagePlan plan{
                .render_frame            = scene_dirty,
                .continue_fade           = false,
                .continue_animation      = false,
                .next_lifetime_deadline  = std::nullopt,
                .next_animation_deadline = std::nullopt,
            };
            for( const auto& record : shapes )
            {
                const auto deadline = shape_deadline( record );
                if( deadline.has_value() && *deadline > now )
                {
                    if( !plan.next_lifetime_deadline.has_value() ||
                        *deadline < *plan.next_lifetime_deadline )
                    {
                        plan.next_lifetime_deadline = *deadline;
                    }
                    if( std::holds_alternative<overlay::Fade>( record.shape.lifetime ) )
                    {
                        plan.render_frame  = true;
                        plan.continue_fade = true;
                    }
                }

                const auto animation = animation_deadline( record );
                if( animation.has_value() && *animation > now )
                {
                    if( !plan.next_animation_deadline.has_value() ||
                        *animation < *plan.next_animation_deadline )
                    {
                        plan.next_animation_deadline = *animation;
                    }
                    plan.render_frame       = true;
                    plan.continue_animation = true;
                }
            }
            return plan;
        }

        Result<void>
        apply_input_passthrough( xcb_connection_t* connection,
                                 xcb_window_t      window )
        {
            return apply_xfixes_input_region( connection,
                                              window,
                                              std::span<const xcb_rectangle_t>{} );
        }

    }    // namespace detail

}    // namespace grab::drivers::desktop::x11

namespace grab::drivers::desktop::x11
{
    namespace
    {

        enum class DelegateState : std::uint8_t
        {
            Closed,
            Synced,
            Desynced,
        };

        [[nodiscard]]
        bool
        shape_order_less( const overlay::ShapeRecord& left,
                          const overlay::ShapeRecord& right ) noexcept
        {
            return std::tie( left.shape.band, left.shape.z, left.id.slot ) <
                   std::tie( right.shape.band, right.shape.z, right.id.slot );
        }

        [[nodiscard]]
        Result<std::size_t>
        native_stride( std::uint16_t       width,
                       const NativeLayout& layout )
        {
            if( layout.bits_per_pixel == 0U || layout.scanline_pad == 0U )
            {
                return fail( ErrorCode::ProtocolError,
                             "X11 overlay visual has an invalid pixmap layout" );
            }
            const auto bits = static_cast<std::size_t>( width ) *
                              static_cast<std::size_t>( layout.bits_per_pixel );
            const auto pad  = static_cast<std::size_t>( layout.scanline_pad );
            if( bits > std::numeric_limits<std::size_t>::max() - ( pad - 1U ) )
            {
                return fail( ErrorCode::Overflowed,
                             "X11 overlay native stride overflowed" );
            }
            return ( ( bits + pad - 1U ) / pad ) * ( pad / bitsPerByte );
        }

        [[nodiscard]]
        Result<std::size_t>
        native_buffer_size( std::size_t   stride,
                            std::uint16_t height )
        {
            if( height !=
                0U &&
                stride >
                std::numeric_limits<std::size_t>::max() /
                static_cast<std::size_t>( height ) )
            {
                return fail( ErrorCode::Overflowed,
                             "X11 overlay native image size overflowed" );
            }
            return stride * static_cast<std::size_t>( height );
        }

        [[nodiscard]]
        std::optional<geometry::Rectangle>
        clipped_damage( const geometry::Rectangle& damage,
                        std::uint16_t              width,
                        std::uint16_t              height ) noexcept;

        [[nodiscard]]
        std::uint32_t
        channel_bits( std::uint8_t  channel,
                      std::uint32_t mask ) noexcept
        {
            if( mask == 0U )
            {
                return 0U;
            }
            const auto shift   = std::countr_zero( mask );
            const auto maximum = mask >> shift;
            const auto scaled  = ( ( static_cast<std::uint64_t>( channel ) * maximum ) +
                                   ( fullChannel / 2U ) ) /
                                 fullChannel;
            return ( static_cast<std::uint32_t>( scaled ) << shift ) & mask;
        }

        // The image and XCB buffers are validated immediately before this tightly
        // bounded pixel conversion loop.
        // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        void
        write_native_pixel( std::byte*    destination,
                            std::uint8_t  byte_count,
                            std::uint8_t  byte_order,
                            std::uint32_t value ) noexcept
        {
            if( byte_order == XCB_IMAGE_ORDER_MSB_FIRST )
            {
                for( std::uint8_t index{}; index < byte_count; ++index )
                {
                    const auto shift =
                        static_cast<std::uint32_t>( byte_count - index - 1U ) *
                        bitsPerByte;
                    destination[index] = static_cast<std::byte>(
                        static_cast<std::uint8_t>( value >> shift )
                    );
                }
                return;
            }
            for( std::uint8_t index{}; index < byte_count; ++index )
            {
                const auto shift   = static_cast<std::uint32_t>( index ) * bitsPerByte;
                destination[index] = static_cast<std::byte>(
                    static_cast<std::uint8_t>( value >> shift )
                );
            }
        }

        [[nodiscard]]
        Result<void>
        convert_image( const Image&                         source,
                       const NativeLayout&                  layout,
                       std::uint16_t                        surface_width,
                       std::uint16_t                        surface_height,
                       std::size_t                          destination_stride,
                       std::span<std::byte>                 destination,
                       std::span<const geometry::Rectangle> damage,
                       bool                                 memcpy_eligible )
        {
            if( source.format !=
                PixelFormat::Bgra ||
                source.width !=
                surface_width ||
                source.height !=
                surface_height ||
                layout.bits_per_pixel %
                bitsPerByte != 0U )
            {
                return fail( ErrorCode::ProtocolError,
                             "X11 overlay renderer returned an incompatible image" );
            }
            const auto bytes_per_pixel =
                static_cast<std::uint8_t>( layout.bits_per_pixel / bitsPerByte );
            if( bytes_per_pixel == 0U || bytes_per_pixel > sizeof( std::uint32_t ) )
            {
                return fail( ErrorCode::ProtocolError,
                             "X11 overlay visual pixel width is unsupported" );
            }
            const auto source_row_bytes =
                static_cast<std::size_t>( surface_width ) * bgraBytesPerPixel;
            const auto destination_row_bytes =
                static_cast<std::size_t>( surface_width ) * bytes_per_pixel;
            if( static_cast<std::size_t>( source.stride ) <
                source_row_bytes ||
                destination_stride < destination_row_bytes )
            {
                return fail( ErrorCode::ProtocolError,
                             "X11 overlay image stride is too short" );
            }
            const auto required =
                destination_stride * static_cast<std::size_t>( surface_height );
            if( destination.size() < required )
            {
                return fail( ErrorCode::ProtocolError,
                             "X11 overlay native pixel storage is too short" );
            }

            const bool use_memcpy =
                memcpy_eligible && bgra_row_strides_compatible( surface_width,
                                                                source.stride,
                                                                destination_stride );
            for( const auto& rectangle : damage )
            {
                const auto clipped =
                    clipped_damage( rectangle, surface_width, surface_height );
                if( !clipped.has_value() )
                {
                    continue;
                }
                const auto first_x = static_cast<std::uint32_t>( clipped->x );
                const auto first_y = static_cast<std::uint32_t>( clipped->y );
                const auto last_x  = first_x + clipped->width;
                const auto last_y  = first_y + clipped->height;
                for( auto y = first_y; y < last_y; ++y )
                {
                    const auto source_row = source.row( y );
                    if( source_row.size() < source_row_bytes )
                    {
                        return fail( ErrorCode::ProtocolError,
                                     "X11 overlay raster row is too short" );
                    }
                    auto* const destination_row =
                        destination.data() +
                        ( static_cast<std::size_t>( y ) * destination_stride );
                    const auto source_offset =
                        static_cast<std::size_t>( first_x ) * bgraBytesPerPixel;
                    if( use_memcpy )
                    {
                        const auto copy_bytes =
                            static_cast<std::size_t>( clipped->width ) *
                            bgraBytesPerPixel;
                        std::memcpy( destination_row + source_offset,
                                     source_row.data() + source_offset,
                                     copy_bytes );
                        continue;
                    }
                    for( auto x = first_x; x < last_x; ++x )
                    {
                        const auto pixel_source_offset =
                            static_cast<std::size_t>( x ) * bgraBytesPerPixel;
                        const auto blue = std::to_integer<std::uint8_t>(
                            source_row[pixel_source_offset + bgraBlueByteIndex]
                        );
                        const auto green = std::to_integer<std::uint8_t>(
                            source_row[pixel_source_offset + bgraGreenByteIndex]
                        );
                        const auto red = std::to_integer<std::uint8_t>(
                            source_row[pixel_source_offset + bgraRedByteIndex]
                        );
                        const auto alpha = std::to_integer<std::uint8_t>(
                            source_row[pixel_source_offset + bgraAlphaByteIndex]
                        );
                        const std::uint32_t pixel =
                            channel_bits( red, layout.red_mask ) |
                            channel_bits( green, layout.green_mask ) |
                            channel_bits( blue, layout.blue_mask ) |
                            channel_bits( alpha, layout.alpha_mask );
                        write_native_pixel( destination_row +
                                                ( static_cast<std::size_t>( x ) *
                                                  bytes_per_pixel ),
                                            bytes_per_pixel,
                                            layout.image_byte_order,
                                            pixel );
                    }
                }
            }
            return {};
        }

        // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

        [[nodiscard]]
        std::optional<geometry::Rectangle>
        clipped_damage( const geometry::Rectangle& damage,
                        std::uint16_t              width,
                        std::uint16_t              height ) noexcept
        {
            const auto left = std::clamp<std::int64_t>( damage.x, 0, width );
            const auto top  = std::clamp<std::int64_t>( damage.y, 0, height );
            const auto right =
                std::clamp<std::int64_t>( static_cast<std::int64_t>( damage.x ) +
                                              damage.width,
                                          0,
                                          width );
            const auto bottom =
                std::clamp<std::int64_t>( static_cast<std::int64_t>( damage.y ) +
                                              damage.height,
                                          0,
                                          height );
            if( right <= left || bottom <= top )
            {
                return std::nullopt;
            }
            return geometry::Rectangle{
                .x      = static_cast<std::int32_t>( left ),
                .y      = static_cast<std::int32_t>( top ),
                .width  = static_cast<std::uint32_t>( right - left ),
                .height = static_cast<std::uint32_t>( bottom - top ),
            };
        }

        [[nodiscard]]
        Result<std::vector<xcb_rectangle_t>>
        native_input_region( std::span<const geometry::Rectangle> rectangles,
                             std::uint16_t                        screen_width,
                             std::uint16_t                        screen_height )
        {
            if( rectangles.size() > std::numeric_limits<std::uint32_t>::max() )
            {
                return fail( ErrorCode::Overflowed,
                             "X11 overlay input region has too many rectangles" );
            }

            std::vector<xcb_rectangle_t> result;
            try
            {
                result.reserve( rectangles.size() );
                const auto right_limit =
                    std::min<std::int64_t>( screen_width, nativeCoordinateExtent );
                const auto bottom_limit =
                    std::min<std::int64_t>( screen_height, nativeCoordinateExtent );
                for( const auto& rectangle : rectangles )
                {
                    const auto source_left = static_cast<std::int64_t>( rectangle.x );
                    const auto source_top  = static_cast<std::int64_t>( rectangle.y );
                    const auto source_right =
                        source_left + static_cast<std::int64_t>( rectangle.width );
                    const auto source_bottom =
                        source_top + static_cast<std::int64_t>( rectangle.height );
                    const auto left =
                        std::clamp<std::int64_t>( source_left, 0, right_limit );
                    const auto top =
                        std::clamp<std::int64_t>( source_top, 0, bottom_limit );
                    const auto right =
                        std::clamp<std::int64_t>( source_right, 0, right_limit );
                    const auto bottom =
                        std::clamp<std::int64_t>( source_bottom, 0, bottom_limit );
                    if( right <= left || bottom <= top )
                    {
                        continue;
                    }
                    result.push_back( xcb_rectangle_t{
                        .x      = static_cast<std::int16_t>( left ),
                        .y      = static_cast<std::int16_t>( top ),
                        .width  = static_cast<std::uint16_t>( right - left ),
                        .height = static_cast<std::uint16_t>( bottom - top ),
                    } );
                }
            }
            catch( const std::bad_alloc& )
            {
                return fail( ErrorCode::Overflowed,
                             "X11 overlay input region allocation failed" );
            }
            catch( const std::length_error& )
            {
                return fail( ErrorCode::Overflowed,
                             "X11 overlay input region exceeds limits" );
            }
            return result;
        }

    }    // namespace

    class X11OverlayDelegate::Impl final
        : public std::enable_shared_from_this<X11OverlayDelegate::Impl>
    {
        public:

            Impl( OwnedConnection                    connection,
                  int                                screen_index,
                  core::Reactor*                     reactor,
                  kernel::scheduling::PacingGovernor governor ) noexcept :
                connection_{ std::move( connection ) },
                screen_index_{ screen_index },
                reactor_{ reactor },
                governor_{ governor }
            {
            }

            ~Impl()
            {
                close_without_notification();
            }

            Impl( const Impl& ) = delete;
            Impl&
            operator=( const Impl& ) = delete;
            Impl( Impl&& )           = delete;
            Impl&
            operator=( Impl&& ) = delete;

            [[nodiscard]]
            Result<void>
            open( CoordinateSpaceId space,
                  bool              require_compositor,
                  bool              map_window )
            {
                if( state_ != DelegateState::Closed )
                {
                    return fail( ErrorCode::InvalidArgument,
                                 "X11 overlay delegate is already open" );
                }
                auto refreshed = probe_connection( connection_.get(),
                                                   screen_index_,
                                                   require_compositor );
                if( !refreshed.has_value() )
                {
                    last_error_ = refreshed.error();
                    if( require_compositor )
                    {
                        notify_availability( false );
                    }
                    return std::unexpected( std::move( refreshed.error() ) );
                }
                probe_ = *refreshed;

                if( map_window && reactor_ == nullptr )
                {
                    // A mapped overlay without a reactor has no compositor
                    // monitor and no lifetime/animation frame clock: compositor loss
                    // would leave a fullscreen opaque window on screen. The
                    // capability requires a bound reactor to map. Checked
                    // after the probe so missing-prerequisite reasons
                    // (ARGB/XFixes/compositor) keep their distinct errors.
                    auto unavailable = capability_error( reactorRequiredReason );
                    last_error_      = unavailable;
                    notify_availability( false );
                    return std::unexpected( std::move( unavailable ) );
                }

                auto created = create_surface();
                if( !created.has_value() )
                {
                    last_error_ = created.error();
                    destroy_surface();
                    return created;
                }

                if( require_compositor )
                {
                    auto monitored = install_event_monitor();
                    if( !monitored.has_value() )
                    {
                        last_error_ = monitored.error();
                        destroy_surface();
                        return monitored;
                    }

                    // Close the probe/subscription race: ownership may have
                    // disappeared while the surface and XFixes subscription
                    // were being created.
                    auto owner = selection_owner( connection_.get(),
                                                  probe_.compositor_selection );
                    if( !owner.has_value() )
                    {
                        last_error_ = owner.error();
                        remove_event_monitor();
                        destroy_surface();
                        return std::unexpected( std::move( owner.error() ) );
                    }
                    if( *owner == XCB_WINDOW_NONE )
                    {
                        auto unavailable =
                            capability_error( compositorUnavailableReason );
                        last_error_ = unavailable;
                        notify_availability( false );
                        remove_event_monitor();
                        destroy_surface();
                        return std::unexpected( std::move( unavailable ) );
                    }
                    probe_.compositor_owner = *owner;
                }

                if( map_window )
                {
                    auto mapped = map_surface();
                    if( !mapped.has_value() )
                    {
                        last_error_ = mapped.error();
                        remove_event_monitor();
                        destroy_surface();
                        return mapped;
                    }
                }

                state_ = DelegateState::Synced;
                space_ = space;
                epoch_.reset();
                accepted_revision_  = {};
                presented_revision_ = {};
                shapes_.clear();
                dirty_             = true;
                surface_presented_ = false;
                should_map_        = map_window;
                compositor_lost_   = false;
                last_error_.reset();
                next_frame_deadline_ =
                    governor_.next_deadline( std::chrono::steady_clock::now() );
                has_frame_deadline_ = true;
                if( require_compositor )
                {
                    notify_availability( true );
                }
                schedule_wakeup();
                return {};
            }

            [[nodiscard]]
            Result<void>
            apply( std::span<const overlay::SceneDelta> deltas )
            {
                if( state_ == DelegateState::Closed )
                {
                    return fail( ErrorCode::InvalidArgument,
                                 "X11 overlay delegate is closed" );
                }
                if( state_ == DelegateState::Desynced )
                {
                    return resync_required();
                }

                auto candidate_shapes   = shapes_;
                auto candidate_epoch    = epoch_;
                auto candidate_revision = accepted_revision_;
                bool changed{};
                for( const auto& delta : deltas )
                {
                    // A Clear delta opening a new epoch IS the explicit epoch
                    // transition: applied atomically, never treated as a gap.
                    if( candidate_epoch.has_value() &&
                        delta.epoch !=
                        *candidate_epoch &&
                        std::holds_alternative<overlay::Clear>( delta.change ) &&
                        delta.revision.value == revisionStep )
                    {
                        candidate_shapes.clear();
                        candidate_epoch    = delta.epoch;
                        candidate_revision = delta.revision;
                        changed            = true;
                        continue;
                    }
                    if( candidate_epoch.has_value() && delta.epoch != *candidate_epoch )
                    {
                        return mark_desynced( "X11 overlay scene epoch changed" );
                    }
                    if( !candidate_epoch.has_value() )
                    {
                        if( delta.revision.value != revisionStep )
                        {
                            return mark_desynced(
                                "X11 overlay delta stream did not start at revision 1"
                            );
                        }
                        candidate_epoch = delta.epoch;
                    }
                    else if( delta.revision <= candidate_revision )
                    {
                        continue;
                    }
                    else if( delta.revision.value -
                             candidate_revision.value != revisionStep )
                    {
                        return mark_desynced(
                            "X11 overlay delta revision is non-contiguous"
                        );
                    }

                    apply_change( candidate_shapes, delta );
                    candidate_revision = delta.revision;
                    changed            = true;
                }

                if( changed )
                {
                    std::ranges::sort( candidate_shapes, shape_order_less );
                    shapes_            = std::move( candidate_shapes );
                    epoch_             = candidate_epoch;
                    accepted_revision_ = candidate_revision;
                    dirty_             = true;
                    ensure_frame_deadline();
                    schedule_wakeup();
                }
                return {};
            }

            [[nodiscard]]
            Result<void>
            resync( const overlay::SceneSnapshot& scene )
            {
                if( state_ == DelegateState::Closed )
                {
                    return fail( ErrorCode::InvalidArgument,
                                 "cannot resync a closed X11 overlay delegate" );
                }

                auto compositor =
                    selection_owner( connection_.get(), probe_.compositor_selection );
                if( !compositor.has_value() )
                {
                    state_      = DelegateState::Desynced;
                    last_error_ = compositor.error();
                    return std::unexpected( std::move( compositor.error() ) );
                }
                if( should_map_ && *compositor == XCB_WINDOW_NONE )
                {
                    auto unavailable = capability_error( compositorUnavailableReason );
                    const bool grab_invalidated = pointer_grabbed_;
                    release_pointer_best_effort();
                    if( mapped_ )
                    {
                        xcb_unmap_window( connection_.get(), window_ );
                        xcb_flush( connection_.get() );
                        mapped_ = false;
                    }
                    state_           = DelegateState::Desynced;
                    last_error_      = unavailable;
                    compositor_lost_ = true;
                    notify_availability( false );
                    if( grab_invalidated )
                    {
                        notify_edit_surface_invalidated( true );
                    }
                    return std::unexpected( std::move( unavailable ) );
                }
                probe_.compositor_owner = *compositor;

                bool grab_invalidated{};
                if( should_map_ && compositor_lost_ )
                {
                    grab_invalidated = pointer_grabbed_;
                    destroy_surface();
                    auto recreated = create_surface();
                    if( !recreated.has_value() )
                    {
                        destroy_surface();
                        if( grab_invalidated )
                        {
                            notify_edit_surface_invalidated( true );
                        }
                        return mark_desynced( std::move( recreated.error() ) );
                    }
                }

                shapes_ = scene.shapes;
                std::ranges::sort( shapes_, shape_order_less );
                epoch_             = scene.epoch;
                accepted_revision_ = scene.through_revision;
                dirty_             = true;
                state_             = DelegateState::Synced;
                last_error_.reset();
                if( should_map_ && !mapped_ )
                {
                    auto mapped = map_surface();
                    if( !mapped.has_value() )
                    {
                        if( grab_invalidated )
                        {
                            notify_edit_surface_invalidated( true );
                        }
                        return mark_desynced( std::move( mapped.error() ) );
                    }
                }
                compositor_lost_ = false;
                notify_availability( true );
                ensure_frame_deadline();
                schedule_wakeup();
                if( grab_invalidated )
                {
                    // resync() is called while the owning service holds its lock.
                    notify_edit_surface_invalidated( true );
                }
                return {};
            }

            [[nodiscard]]
            Result<void>
            flush( overlay::Revision through )
            {
                if( state_ == DelegateState::Closed )
                {
                    return fail( ErrorCode::InvalidArgument,
                                 "cannot flush a closed X11 overlay delegate" );
                }
                drain_events( true );
                if( state_ == DelegateState::Desynced )
                {
                    return resync_required();
                }
                if( through > accepted_revision_ )
                {
                    return fail( ErrorCode::InvalidArgument,
                                 "cannot flush an unapplied X11 overlay revision" );
                }

                if( !surface_presented_ || dirty_ || presented_revision_ < through )
                {
                    auto presented = present_tick();
                    if( !presented.has_value() )
                    {
                        return presented;
                    }
                }
                auto fenced = sync_fence();
                if( !fenced.has_value() )
                {
                    return mark_desynced( std::move( fenced.error() ) );
                }
                drain_events( true );
                if( state_ == DelegateState::Desynced )
                {
                    return resync_required();
                }
                return {};
            }

            [[nodiscard]]
            Result<void>
            set_input_region( std::span<const geometry::Rectangle> rectangles )
            {
                if( state_ == DelegateState::Closed || window_ == XCB_WINDOW_NONE )
                {
                    if( rectangles.empty() )
                    {
                        input_region_.clear();
                        return {};
                    }
                    return fail( ErrorCode::InvalidArgument,
                                 "X11 overlay input region requires an open window" );
                }

                auto candidate = native_input_region( rectangles,
                                                      probe_.screen.width,
                                                      probe_.screen.height );
                if( !candidate.has_value() )
                {
                    return std::unexpected( std::move( candidate.error() ) );
                }
                auto applied = apply_input_region( *candidate );
                if( !applied.has_value() )
                {
                    return applied;
                }
                input_region_ = std::move( *candidate );
                return {};
            }

            [[nodiscard]]
            Result<void>
            set_edit_handler( spi::OverlayEditHandler handler )
            {
                if( state_ == DelegateState::Closed || window_ == XCB_WINDOW_NONE )
                {
                    if( !handler )
                    {
                        edit_handler_ = {};
                        return {};
                    }
                    return fail( ErrorCode::InvalidArgument,
                                 "X11 overlay edit handler requires an open window" );
                }
                if( handler && reactor_ == nullptr )
                {
                    return fail( ErrorCode::CapabilityUnavailable,
                                 "X11 overlay edit events require a bound reactor" );
                }

                const std::array<std::uint32_t, 1U> values{
                    handler ? editWindowEventMask : passiveWindowEventMask,
                };
                auto selected = check_request(
                    connection_.get(),
                    xcb_change_window_attributes_checked( connection_.get(),
                                                          window_,
                                                          XCB_CW_EVENT_MASK,
                                                          values.data() ),
                    handler ? "select X11 overlay edit events"
                            : "clear X11 overlay edit events"
                );
                if( !selected.has_value() )
                {
                    return selected;
                }
                edit_handler_ = std::move( handler );
                return {};
            }

            // Extent of the overlay surface, which spans the probed screen.
            [[nodiscard]]
            geometry::Rectangle
            surface_bounds() const noexcept
            {
                return geometry::Rectangle{
                    .x      = 0,
                    .y      = 0,
                    .width  = probe_.screen.width,
                    .height = probe_.screen.height,
                };
            }

            [[nodiscard]]
            Result<void>
            grab_pointer()
            {
                if( pointer_grabbed_ )
                {
                    return {};
                }
                if( state_ == DelegateState::Closed || window_ == XCB_WINDOW_NONE )
                {
                    return fail( ErrorCode::InvalidArgument,
                                 "X11 overlay pointer grab requires an open window" );
                }

                // Until a reply proves otherwise, a transport failure leaves
                // the server-side grab state ambiguous.  Keeping the cleanup
                // obligation causes session rollback to issue XUngrabPointer.
                pointer_grabbed_ = true;
                xcb_generic_error_t* raw_error{};
                const auto           reply = take_xcb_owned(
                    xcb_grab_pointer_reply( connection_.get(),
                                            xcb_grab_pointer( connection_.get(),
                                                              doNotUseOwnerEvents,
                                                              window_,
                                                              pointerGrabEventMask,
                                                              XCB_GRAB_MODE_ASYNC,
                                                              XCB_GRAB_MODE_ASYNC,
                                                              XCB_WINDOW_NONE,
                                                              XCB_CURSOR_NONE,
                                                              XCB_TIME_CURRENT_TIME ),
                                            &raw_error )
                );
                const auto error = take_xcb_owned( raw_error );
                if( error != nullptr || reply == nullptr )
                {
                    return std::unexpected(
                        make_error( ErrorCode::ProtocolError,
                                    "request X11 overlay pointer grab failed" )
                    );
                }
                if( reply->status != XCB_GRAB_STATUS_SUCCESS )
                {
                    pointer_grabbed_ = false;
                    return std::unexpected(
                        make_error( ErrorCode::ProviderFailed,
                                    "X11 overlay pointer grab was refused with status " +
                                        std::to_string( reply->status ) )
                    );
                }
                return {};
            }

            [[nodiscard]]
            Result<void>
            ungrab_pointer()
            {
                if( !pointer_grabbed_ )
                {
                    return {};
                }
                if( connection_ == nullptr )
                {
                    pointer_grabbed_ = false;
                    return {};
                }
                auto released =
                    check_request( connection_.get(),
                                   xcb_ungrab_pointer_checked( connection_.get(),
                                                               XCB_TIME_CURRENT_TIME ),
                                   "release X11 overlay pointer grab" );
                if( !released.has_value() )
                {
                    return released;
                }
                pointer_grabbed_ = false;
                return {};
            }

            void
            close()
            {
                close_without_notification();
            }

            void
            set_availability_changed( AvailabilityChanged callback )
            {
                availability_changed_ = std::move( callback );
            }

            void
            set_topology_refresh( TopologyRefresh callback )
            {
                topology_refresh_ = std::move( callback );
            }

            [[nodiscard]]
            xcb_window_t
            window() const noexcept
            {
                return window_;
            }

            void
            simulate_topology_change( std::uint16_t width,
                                      std::uint16_t height )
            {
                handle_topology_change( width, height, false );
            }

        private:

            struct ShmStorage
            {
                    xcb_shm_seg_t segment{};
                    int           identifier{ systemCallFailure };
                    void*         address{};
                    std::size_t   size{};
                    bool          attached{};
            };

            [[nodiscard]]
            Result<void>
            apply_input_region( std::span<const xcb_rectangle_t> rectangles )
            {
                // open() negotiates XFixes once while populating probe_. Reuse that
                // connection-local result for input-region updates.
                return apply_negotiated_xfixes_input_region( connection_.get(),
                                                             window_,
                                                             rectangles );
            }

            void
            refresh_memcpy_eligibility() noexcept
            {
                const auto source_stride =
                    static_cast<std::size_t>( probe_.screen.width ) * bgraBytesPerPixel;
                native_memcpy_eligible_ =
                    native_bgra_memcpy_compatible( probe_.layout,
                                                   probe_.screen.width,
                                                   source_stride,
                                                   native_stride_ );
            }

            [[nodiscard]]
            Result<void>
            create_surface()
            {
                if( probe_.screen.width == 0U || probe_.screen.height == 0U )
                {
                    return fail( ErrorCode::GeometryUntrusted,
                                 "X11 overlay screen has an empty geometry" );
                }

                colormap_ = xcb_generate_id( connection_.get() );
                auto colormap_created =
                    check_request( connection_.get(),
                                   xcb_create_colormap_checked( connection_.get(),
                                                                XCB_COLORMAP_ALLOC_NONE,
                                                                colormap_,
                                                                probe_.screen.root,
                                                                probe_.layout.visual ),
                                   "create X11 overlay ARGB colormap" );
                if( !colormap_created.has_value() )
                {
                    return colormap_created;
                }

                window_ = xcb_generate_id( connection_.get() );
                constexpr std::uint32_t windowMask = XCB_CW_BACK_PIXEL |
                                                     XCB_CW_BORDER_PIXEL |
                                                     XCB_CW_OVERRIDE_REDIRECT |
                                                     XCB_CW_EVENT_MASK |
                                                     XCB_CW_COLORMAP;
                const std::array        values{
                    0U,
                    0U,
                    1U,
                    edit_handler_ ? editWindowEventMask : passiveWindowEventMask,
                    colormap_,
                };
                auto window_created = check_request(
                    connection_.get(),
                    xcb_create_window_checked( connection_.get(),
                                               probe_.layout.depth,
                                               window_,
                                               probe_.screen.root,
                                               0,
                                               0,
                                               probe_.screen.width,
                                               probe_.screen.height,
                                               0U,
                                               XCB_WINDOW_CLASS_INPUT_OUTPUT,
                                               probe_.layout.visual,
                                               windowMask,
                                               values.data() ),
                    "create X11 overlay ARGB window"
                );
                if( !window_created.has_value() )
                {
                    return window_created;
                }

                auto passthrough = apply_input_region( {} );
                if( !passthrough.has_value() )
                {
                    return passthrough;
                }
                if( !input_region_.empty() )
                {
                    auto restored = apply_input_region( input_region_ );
                    if( !restored.has_value() )
                    {
                        return restored;
                    }
                }

                gc_ = xcb_generate_id( connection_.get() );
                constexpr std::uint32_t             gcMask = XCB_GC_GRAPHICS_EXPOSURES;
                const std::array<std::uint32_t, 1U> gc_values{ noEventPropagation };
                auto                                gc_created =
                    check_request( connection_.get(),
                                   xcb_create_gc_checked( connection_.get(),
                                                          gc_,
                                                          window_,
                                                          gcMask,
                                                          gc_values.data() ),
                                   "create X11 overlay graphics context" );
                if( !gc_created.has_value() )
                {
                    return gc_created;
                }

                auto raster =
                    kernel::presentation::OverlayRaster::create( geometry::Size{
                        .width  = probe_.screen.width,
                        .height = probe_.screen.height,
                    } );
                if( !raster.has_value() )
                {
                    return std::unexpected( std::move( raster.error() ) );
                }
                raster_.emplace( std::move( *raster ) );

                auto stride = native_stride( probe_.screen.width, probe_.layout );
                if( !stride.has_value() )
                {
                    return std::unexpected( std::move( stride.error() ) );
                }
                auto size = native_buffer_size( *stride, probe_.screen.height );
                if( !size.has_value() )
                {
                    return std::unexpected( std::move( size.error() ) );
                }
                native_stride_ = *stride;
                auto storage   = allocate_pixel_storage( *size );
                if( !storage.has_value() )
                {
                    return storage;
                }
                return {};
            }

            [[nodiscard]]
            Result<void>
            allocate_pixel_storage( std::size_t size )
            {
                native_memcpy_eligible_ = false;
                release_shm();
                native_pixels_.clear();
                if( probe_.extensions.shm_available && try_allocate_shm( size ) )
                {
                    refresh_memcpy_eligibility();
                    return {};
                }
                try
                {
                    native_pixels_.resize( size );
                }
                catch( const std::bad_alloc& )
                {
                    return fail( ErrorCode::Overflowed,
                                 "X11 overlay pixel allocation failed" );
                }
                catch( const std::length_error& )
                {
                    return fail( ErrorCode::Overflowed,
                                 "X11 overlay pixel allocation exceeds limits" );
                }
                refresh_memcpy_eligibility();
                return {};
            }

            [[nodiscard]]
            bool
            try_allocate_shm( std::size_t size )
            {
                if( size == 0U )
                {
                    return false;
                }
                shm_.identifier =
                    shmget( IPC_PRIVATE, size, IPC_CREAT | shmPermissions );
                if( shm_.identifier == systemCallFailure )
                {
                    return false;
                }
                shm_.address = shmat( shm_.identifier, nullptr, 0 );
                // POSIX specifies the sentinel as `(void*) -1`.
                // NOLINTNEXTLINE(performance-no-int-to-ptr,cppcoreguidelines-pro-type-reinterpret-cast)
                if( shm_.address == reinterpret_cast<void*>( systemCallFailure ) )
                {
                    shmctl( shm_.identifier, IPC_RMID, nullptr );
                    shm_ = {};
                    return false;
                }
                shm_.segment = xcb_generate_id( connection_.get() );
                auto attached =
                    check_request( connection_.get(),
                                   xcb_shm_attach_checked(
                                       connection_.get(),
                                       shm_.segment,
                                       static_cast<std::uint32_t>( shm_.identifier ),
                                       0U
                                   ),
                                   "attach X11 overlay shared memory" );
                if( !attached.has_value() )
                {
                    shmdt( shm_.address );
                    shmctl( shm_.identifier, IPC_RMID, nullptr );
                    shm_ = {};
                    return false;
                }
                shm_.size     = size;
                shm_.attached = true;
                shmctl( shm_.identifier, IPC_RMID, nullptr );
                shm_.identifier = systemCallFailure;
                return true;
            }

            void
            release_shm() noexcept
            {
                if( shm_.attached && connection_ != nullptr )
                {
                    xcb_shm_detach( connection_.get(), shm_.segment );
                }
                if( shm_.address != nullptr )
                {
                    shmdt( shm_.address );
                }
                if( shm_.identifier != systemCallFailure )
                {
                    shmctl( shm_.identifier, IPC_RMID, nullptr );
                }
                shm_ = {};
            }

            void
            destroy_surface() noexcept
            {
                release_pointer_best_effort();
                release_shm();
                native_pixels_.clear();
                raster_.reset();
                if( gc_ != XCB_NONE )
                {
                    xcb_free_gc( connection_.get(), gc_ );
                    gc_ = XCB_NONE;
                }
                if( window_ != XCB_WINDOW_NONE )
                {
                    xcb_destroy_window( connection_.get(), window_ );
                    window_ = XCB_WINDOW_NONE;
                }
                if( colormap_ != XCB_COLORMAP_NONE )
                {
                    xcb_free_colormap( connection_.get(), colormap_ );
                    colormap_ = XCB_COLORMAP_NONE;
                }
                mapped_                 = false;
                native_stride_          = 0U;
                native_memcpy_eligible_ = false;
                surface_presented_      = false;
                if( connection_ != nullptr )
                {
                    xcb_flush( connection_.get() );
                }
            }

            void
            release_pointer_best_effort() noexcept
            {
                if( !pointer_grabbed_ || connection_ == nullptr )
                {
                    return;
                }
                xcb_ungrab_pointer( connection_.get(), XCB_TIME_CURRENT_TIME );
                pointer_grabbed_ = false;
            }

            [[nodiscard]]
            Result<void>
            install_event_monitor()
            {
                constexpr std::uint32_t rootEventMask =
                    XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
                const std::array<std::uint32_t, 1U> root_events{ rootEventMask };
                auto                                selected_root = check_request(
                    connection_.get(),
                    xcb_change_window_attributes_checked( connection_.get(),
                                                          probe_.screen.root,
                                                          XCB_CW_EVENT_MASK,
                                                          root_events.data() ),
                    "select X11 overlay stacking events"
                );
                if( !selected_root.has_value() )
                {
                    return selected_root;
                }

                constexpr std::uint32_t selectionEvents =
                    XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER |
                    XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
                    XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE;
                auto selected_owner =
                    check_request( connection_.get(),
                                   xcb_xfixes_select_selection_input_checked(
                                       connection_.get(),
                                       probe_.screen.root,
                                       probe_.compositor_selection,
                                       selectionEvents
                                   ),
                                   "select X11 overlay compositor ownership events" );
                if( !selected_owner.has_value() )
                {
                    return selected_owner;
                }

                if( probe_.extensions.randr_available )
                {
                    auto selected_randr =
                        check_request( connection_.get(),
                                       xcb_randr_select_input_checked(
                                           connection_.get(),
                                           probe_.screen.root,
                                           XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE
                                       ),
                                       "select X11 overlay RandR screen-change events" );
                    if( !selected_randr.has_value() )
                    {
                        return selected_randr;
                    }
                }

                if( reactor_ != nullptr )
                {
                    const int descriptor = xcb_get_file_descriptor( connection_.get() );
                    if( descriptor == invalidFileDescriptor )
                    {
                        return fail( ErrorCode::DeviceInaccessible,
                                     "X11 overlay connection fd is unavailable" );
                    }
                    const std::weak_ptr<Impl> weak = weak_from_this();
                    try
                    {
                        event_token_ = reactor_->add_fd(
                            descriptor,
                            static_cast<std::uint32_t>( EPOLLIN | EPOLLERR | EPOLLHUP ),
                            [weak]( std::uint32_t )
                            {
                                if( const auto impl = weak.lock() )
                                {
                                    impl->drain_events( false );
                                }
                            }
                        );
                    }
                    catch( const std::exception& error )
                    {
                        return fail( ErrorCode::InternalFault,
                                     std::string{
                                         "install X11 overlay reactor monitor: "
                                     } + error.what() );
                    }
                    catch( ... )
                    {
                        return fail( ErrorCode::InternalFault,
                                     "install X11 overlay reactor monitor failed" );
                    }
                }
                return {};
            }

            void
            remove_event_monitor() noexcept
            {
                if( reactor_ != nullptr && event_token_.has_value() )
                {
                    try
                    {
                        reactor_->remove_fd( *event_token_ );
                    }
                    catch( ... )
                    {
                        // Best-effort teardown; callbacks capture only weak state.
                        event_token_.reset();
                    }
                }
                event_token_.reset();
            }

            [[nodiscard]]
            Result<void>
            map_surface()
            {
                auto mapped =
                    check_request( connection_.get(),
                                   xcb_map_window_checked( connection_.get(), window_ ),
                                   "map X11 overlay window" );
                if( !mapped.has_value() )
                {
                    return mapped;
                }
                auto raised = raise_surface();
                if( !raised.has_value() )
                {
                    xcb_unmap_window( connection_.get(), window_ );
                    xcb_flush( connection_.get() );
                    return raised;
                }
                if( xcb_flush( connection_.get() ) <= flushFailure )
                {
                    xcb_unmap_window( connection_.get(), window_ );
                    xcb_flush( connection_.get() );
                    return fail( ErrorCode::DeviceInaccessible,
                                 "map X11 overlay window flush failed" );
                }
                mapped_ = true;
                return {};
            }

            [[nodiscard]]
            Result<void>
            raise_surface()
            {
                constexpr std::uint32_t configureMask = XCB_CONFIG_WINDOW_STACK_MODE;
                const std::array<std::uint32_t, 1U> values{ XCB_STACK_MODE_ABOVE };
                return check_request( connection_.get(),
                                      xcb_configure_window_checked( connection_.get(),
                                                                    window_,
                                                                    configureMask,
                                                                    values.data() ),
                                      "raise X11 overlay window" );
            }

            static void
            apply_change( std::vector<overlay::ShapeRecord>& shapes,
                          const overlay::SceneDelta&         delta )
            {
                if( const auto* upsert = std::get_if<overlay::Upsert>( &delta.change ) )
                {
                    const auto existing = std::ranges::find( shapes,
                                                             upsert->record.id,
                                                             &overlay::ShapeRecord::id );
                    if( existing == shapes.end() )
                    {
                        shapes.push_back( upsert->record );
                    }
                    else
                    {
                        *existing = upsert->record;
                    }
                    return;
                }
                if( const auto* remove = std::get_if<overlay::Remove>( &delta.change ) )
                {
                    std::erase_if( shapes,
                                   [remove]( const overlay::ShapeRecord& record )
                                   {
                                       return record.id == remove->id;
                                   } );
                    return;
                }
                shapes.clear();
            }

            void
            ensure_frame_deadline()
            {
                if( !has_frame_deadline_ )
                {
                    next_frame_deadline_ =
                        governor_.next_deadline( std::chrono::steady_clock::now() );
                    has_frame_deadline_ = true;
                }
            }

            void
            schedule_wakeup()
            {
                if( state_ == DelegateState::Closed || reactor_ == nullptr )
                {
                    return;
                }

                const auto now_ms = monotonic_milliseconds();
                const auto plan = detail::overlay_damage_plan( shapes_, now_ms, dirty_ );
                if( plan.render_frame )
                {
                    ensure_frame_deadline();
                }
                else
                {
                    has_frame_deadline_ = false;
                }

                std::optional<std::chrono::steady_clock::time_point> next;
                if( has_frame_deadline_ )
                {
                    next = next_frame_deadline_;
                }
                if( plan.next_lifetime_deadline.has_value() )
                {
                    const auto lifetime = std::chrono::steady_clock::time_point{
                        *plan.next_lifetime_deadline
                    };
                    if( !next.has_value() || lifetime < *next )
                    {
                        next = lifetime;
                    }
                }
                if( plan.next_animation_deadline.has_value() )
                {
                    const auto animation = std::chrono::steady_clock::time_point{
                        *plan.next_animation_deadline
                    };
                    if( !next.has_value() || animation < *next )
                    {
                        next = animation;
                    }
                }
                if( !next.has_value() )
                {
                    scheduled_wakeup_.reset();
                    ++timer_serial_;
                    return;
                }
                if( scheduled_wakeup_.has_value() && *scheduled_wakeup_ <= *next )
                {
                    return;
                }

                scheduled_wakeup_ = *next;
                ++timer_serial_;
                const auto serial = timer_serial_;
                const auto now    = std::chrono::steady_clock::now();
                const auto delay  = *next > now ? *next - now : noTimerDelay;
                const std::weak_ptr<Impl> weak = weak_from_this();
                try
                {
                    reactor_->add_timer( delay,
                                         [weak, serial]
                                         {
                                             if( const auto impl = weak.lock() )
                                             {
                                                 impl->handle_wakeup( serial );
                                             }
                                         } );
                }
                catch( const std::exception& error )
                {
                    scheduled_wakeup_.reset();
                    mark_async_desynced( make_error(
                        ErrorCode::InternalFault,
                        std::string{ "schedule X11 overlay frame: " } + error.what()
                    ) );
                }
                catch( ... )
                {
                    scheduled_wakeup_.reset();
                    mark_async_desynced(
                        make_error( ErrorCode::InternalFault,
                                    "schedule X11 overlay frame failed" )
                    );
                }
            }

            void
            handle_wakeup( std::uint64_t serial )
            {
                if( serial != timer_serial_ || state_ == DelegateState::Closed )
                {
                    return;
                }
                scheduled_wakeup_.reset();
                drain_events( false );
                if( state_ != DelegateState::Synced )
                {
                    return;
                }

                const auto fired_at  = std::chrono::steady_clock::now();
                auto       presented = present_tick();
                if( !presented.has_value() )
                {
                    mark_async_desynced( std::move( presented.error() ) );
                    return;
                }
                if( has_frame_deadline_ )
                {
                    next_frame_deadline_ =
                        governor_.next_deadline( next_frame_deadline_ );
                    while( next_frame_deadline_ <= fired_at )
                    {
                        next_frame_deadline_ =
                            governor_.next_deadline( next_frame_deadline_ );
                    }
                }
                schedule_wakeup();
            }

            [[nodiscard]]
            std::span<std::byte>
            native_storage()
            {
                if( shm_.attached )
                {
                    return std::span<std::byte>{
                        static_cast<std::byte*>( shm_.address ),
                        shm_.size,
                    };
                }
                return native_pixels_;
            }

            [[nodiscard]]
            std::span<const std::byte>
            native_storage() const
            {
                if( shm_.attached )
                {
                    return std::span<const std::byte>{
                        static_cast<const std::byte*>( shm_.address ),
                        shm_.size,
                    };
                }
                return native_pixels_;
            }

            [[nodiscard]]
            Result<void>
            present_tick()
            {
                if( raster_ == std::nullopt )
                {
                    return fail( ErrorCode::InternalFault,
                                 "X11 overlay raster is unavailable" );
                }

                // The four phases of a frame, each timed separately. Against a
                // 16.7 ms budget, "the overlay is laggy" is only actionable
                // once you know which of these spent it.
                diag::FrameSample                      sample{};

                const diag::Scope<log::Level::Nominal> raster_scope;
                auto frame    = raster_->render( shapes_, monotonic_milliseconds() );
                sample.raster = raster_scope.elapsed();
                if( !frame.has_value() )
                {
                    return std::unexpected( std::move( frame.error() ) );
                }

                if( !frame->damage.empty() )
                {
                    sample.damaged_pixels = damaged_pixel_count( frame->damage );

                    const diag::Scope<log::Level::Nominal> convert_scope;
                    auto converted = convert_image( frame->pixels,
                                                    probe_.layout,
                                                    probe_.screen.width,
                                                    probe_.screen.height,
                                                    native_stride_,
                                                    native_storage(),
                                                    frame->damage,
                                                    native_memcpy_eligible_ );
                    sample.convert = convert_scope.elapsed();
                    if( !converted.has_value() )
                    {
                        return converted;
                    }

                    // With full_present, the damage still drives the raster
                    // and the conversion (the native/SHM mirror is a
                    // persistent full-surface copy), but the PUT covers the
                    // whole surface: on a PRIME-sink display, the server-side
                    // dirty region is then the full screen and every sink
                    // copy self-heals whatever stale overlay content the
                    // glass was holding. Idle frames still present nothing.
                    const std::array<geometry::Rectangle, 1U> whole_surface{
                        geometry::Rectangle{
                                            .x      = 0,
                                            .y      = 0,
                                            .width  = probe_.screen.width,
                                            .height = probe_.screen.height,
                                            },
                    };
                    const std::span<const geometry::Rectangle> to_present =
                        probe_.full_present
                            ? std::span<const geometry::Rectangle>{ whole_surface }
                            : std::span<const geometry::Rectangle>{ frame->damage };

                    const diag::Scope<log::Level::Nominal> present_scope;
                    auto presented = shm_.attached ? present_shm( to_present )
                                                   : present_put_image( to_present );
                    sample.present = present_scope.elapsed();
                    if( !presented.has_value() )
                    {
                        return presented;
                    }

                    log::verbose(
                        [&frame, &sample, this]( auto& event )
                        {
                            event.tag( log::tags::present )
                                .value( "damage_rects", frame->damage.size() )
                                .value( "damage_px", sample.damaged_pixels )
                                .value( "route", shm_.attached ? "shm" : "put_image" )
                                .value( "policy",
                                        probe_.full_present ? "full" : "incremental" )
                                .value( "memcpy_fast_path", native_memcpy_eligible_ )
                                .value( "revision", accepted_revision_.value );
                        }
                    );
                }

                const diag::Scope<log::Level::Nominal> flush_scope;
                const bool                             flush_failed =
                    xcb_flush( connection_.get() ) <=
                    flushFailure ||
                    xcb_connection_has_error( connection_.get() ) != xcbSuccess;
                sample.flush = flush_scope.elapsed();
                if( flush_failed )
                {
                    return fail( ErrorCode::DeviceInaccessible,
                                 "X11 overlay presentation flush failed" );
                }

                sample.events_drained   = frame_events_drained_;
                sample.input_to_present = input_to_present_latency();
                // Cleared so a frame driven by no new input reports zero
                // latency rather than re-reporting the previous frame's.
                frame_events_drained_ = 0;
                frame_input_server_ms_.reset();

                diag::record_frame( sample );
                if( diag::due_for_report() )
                {
                    diag::log_report( diag::report(), log::Level::Verbose );
                }

                dirty_              = false;
                surface_presented_  = true;
                presented_revision_ = accepted_revision_;
                return {};
            }

            // Total pixels in the damage set, for the frame report. Damage
            // rectangles may overlap, so this is an upper bound on the work
            // done rather than an exact count of distinct pixels — which is
            // the quantity that matters for "how much did this frame paint".
            [[nodiscard]]
            static std::uint32_t
            damaged_pixel_count( std::span<const geometry::Rectangle> damage ) noexcept
            {
                std::uint64_t total = 0;
                for( const auto& rectangle : damage )
                {
                    total += static_cast<std::uint64_t>( rectangle.width ) *
                             static_cast<std::uint64_t>( rectangle.height );
                }
                return static_cast<std::uint32_t>(
                    std::min<std::uint64_t>( total,
                                             std::numeric_limits<std::uint32_t>::max() )
                );
            }

            // Server-timestamp of the newest input this frame reflects, to now.
            // Zero when the clock is uncalibrated or no input drove this frame:
            // reporting a number derived from an uncalibrated server clock
            // would be confidently wrong.
            [[nodiscard]]
            std::chrono::nanoseconds
            input_to_present_latency() const noexcept
            {
                if( !frame_input_server_ms_.has_value() )
                {
                    return {};
                }
                const auto instant = server_clock_.instant_of( *frame_input_server_ms_ );
                if( !instant.has_value() )
                {
                    return {};
                }
                return std::chrono::steady_clock::now() - *instant;
            }

            [[nodiscard]]
            Result<void>
            present_shm( std::span<const geometry::Rectangle> damage )
            {
                for( const auto& rectangle : damage )
                {
                    const auto clipped = clipped_damage( rectangle,
                                                         probe_.screen.width,
                                                         probe_.screen.height );
                    if( !clipped.has_value() )
                    {
                        continue;
                    }
                    auto result =
                        check_request( connection_.get(),
                                       xcb_shm_put_image_checked(
                                           connection_.get(),
                                           window_,
                                           gc_,
                                           probe_.screen.width,
                                           probe_.screen.height,
                                           static_cast<std::uint16_t>( clipped->x ),
                                           static_cast<std::uint16_t>( clipped->y ),
                                           static_cast<std::uint16_t>( clipped->width ),
                                           static_cast<std::uint16_t>( clipped->height ),
                                           static_cast<std::int16_t>( clipped->x ),
                                           static_cast<std::int16_t>( clipped->y ),
                                           probe_.layout.depth,
                                           XCB_IMAGE_FORMAT_Z_PIXMAP,
                                           noShmCompletionEvent,
                                           shm_.segment,
                                           0U
                                       ),
                                       "present X11 overlay shared-memory damage" );
                    if( !result.has_value() )
                    {
                        return result;
                    }
                }
                return {};
            }

            // NOLINTBEGIN(readability-function-size)
            [[nodiscard]]
            Result<void>
            present_put_image( std::span<const geometry::Rectangle> damage )
            {
                const auto bytes_per_pixel =
                    static_cast<std::size_t>( probe_.layout.bits_per_pixel /
                                              bitsPerByte );
                const auto max_request_units =
                    xcb_get_maximum_request_length( connection_.get() );
                const auto max_request_bytes =
                    static_cast<std::size_t>( max_request_units ) *
                    sizeof( std::uint32_t );
                if( max_request_bytes <=
                    sizeof( xcb_put_image_request_t ) ||
                    bytes_per_pixel == 0U )
                {
                    return fail( ErrorCode::ProtocolError,
                                 "X11 overlay PutImage request limit is invalid" );
                }
                const auto max_payload =
                    max_request_bytes - sizeof( xcb_put_image_request_t );
                const auto source = native_storage();

                for( const auto& rectangle : damage )
                {
                    const auto clipped = clipped_damage( rectangle,
                                                         probe_.screen.width,
                                                         probe_.screen.height );
                    if( !clipped.has_value() )
                    {
                        continue;
                    }
                    std::uint32_t x_offset{};
                    while( x_offset < clipped->width )
                    {
                        const auto max_width =
                            std::max<std::size_t>( 1U, max_payload / bytes_per_pixel );
                        const auto tile_width = std::min<std::uint32_t>(
                            clipped->width - x_offset,
                            static_cast<std::uint32_t>( std::min<std::size_t>(
                                max_width,
                                std::numeric_limits<std::uint16_t>::max()
                            ) )
                        );
                        auto tile_stride_result =
                            native_stride( static_cast<std::uint16_t>( tile_width ),
                                           probe_.layout );
                        if( !tile_stride_result.has_value() )
                        {
                            return std::unexpected(
                                std::move( tile_stride_result.error() )
                            );
                        }
                        const auto tile_stride = *tile_stride_result;
                        const auto max_rows =
                            std::max<std::size_t>( 1U, max_payload / tile_stride );
                        std::uint32_t y_offset{};
                        while( y_offset < clipped->height )
                        {
                            const auto tile_height = std::min<std::uint32_t>(
                                clipped->height - y_offset,
                                static_cast<std::uint32_t>( std::min<std::size_t>(
                                    max_rows,
                                    std::numeric_limits<std::uint16_t>::max()
                                ) )
                            );
                            std::vector<std::byte> tile( tile_stride * tile_height,
                                                         std::byte{} );
                            for( std::uint32_t row{}; row < tile_height; ++row )
                            {
                                const auto source_y =
                                    static_cast<std::size_t>( clipped->y ) +
                                    y_offset +
                                    row;
                                const auto source_x =
                                    static_cast<std::size_t>( clipped->x ) + x_offset;
                                const auto source_offset =
                                    ( source_y * native_stride_ ) +
                                    ( source_x * bytes_per_pixel );
                                const auto copy_bytes =
                                    static_cast<std::size_t>( tile_width ) *
                                    bytes_per_pixel;
                                std::ranges::copy_n(
                                    source.begin() +
                                        static_cast<std::ptrdiff_t>( source_offset ),
                                    static_cast<std::ptrdiff_t>( copy_bytes ),
                                    tile.begin() +
                                        static_cast<std::ptrdiff_t>(
                                            static_cast<std::size_t>( row ) * tile_stride
                                        )
                                );
                            }
                            auto result = check_request(
                                connection_.get(),
                                xcb_put_image_checked(
                                    connection_.get(),
                                    XCB_IMAGE_FORMAT_Z_PIXMAP,
                                    window_,
                                    gc_,
                                    static_cast<std::uint16_t>( tile_width ),
                                    static_cast<std::uint16_t>( tile_height ),
                                    static_cast<std::int16_t>(
                                        clipped->x +
                                        static_cast<std::int32_t>( x_offset )
                                    ),
                                    static_cast<std::int16_t>(
                                        clipped->y +
                                        static_cast<std::int32_t>( y_offset )
                                    ),
                                    0U,
                                    probe_.layout.depth,
                                    static_cast<std::uint32_t>( tile.size() ),
                                    // XCB models an immutable byte buffer as uint8_t*.
                                    // NOLINTNEXTLINE(bugprone-bitwise-pointer-cast)
                                    std::bit_cast<const std::uint8_t*>( tile.data() )
                                ),
                                "present X11 overlay PutImage damage"
                            );
                            if( !result.has_value() )
                            {
                                return result;
                            }
                            y_offset += tile_height;
                        }
                        x_offset += tile_width;
                    }
                }
                return {};
            }

            // NOLINTEND(readability-function-size)

            [[nodiscard]]
            Result<void>
            sync_fence()
            {
                const auto cookie = xcb_get_input_focus( connection_.get() );
                if( xcb_flush( connection_.get() ) <= flushFailure )
                {
                    return fail( ErrorCode::DeviceInaccessible,
                                 "X11 overlay flush fence could not be sent" );
                }
                const auto deadline =
                    std::chrono::steady_clock::now() + flushFenceTimeout;
                for( ;; )
                {
                    void*                raw_reply{};
                    xcb_generic_error_t* raw_error{};
                    const int            ready = xcb_poll_for_reply( connection_.get(),
                                                                     cookie.sequence,
                                                                     &raw_reply,
                                                                     &raw_error );
                    const auto           reply = take_xcb_owned(
                        static_cast<xcb_get_input_focus_reply_t*>( raw_reply )
                    );
                    const auto error = take_xcb_owned( raw_error );
                    if( ready != 0 )
                    {
                        if( error != nullptr || reply == nullptr )
                        {
                            return fail( ErrorCode::ProtocolError,
                                         "X11 overlay flush fence failed" );
                        }
                        return {};
                    }

                    const auto now = std::chrono::steady_clock::now();
                    if( now >= deadline )
                    {
                        xcb_discard_reply( connection_.get(), cookie.sequence );
                        return fail( ErrorCode::DeadlineExceeded,
                                     "X11 overlay flush fence timed out" );
                    }
                    const auto remaining =
                        std::chrono::ceil<std::chrono::milliseconds>( deadline - now );
                    const auto timeout =
                        static_cast<int>( std::min<std::chrono::milliseconds::rep>(
                            remaining.count(),
                            std::numeric_limits<int>::max()
                        ) );
                    pollfd target{
                        .fd      = xcb_get_file_descriptor( connection_.get() ),
                        .events  = xcbPollEvents,
                        .revents = noPollEvents,
                    };
                    const int polled =
                        poll( &target, static_cast<nfds_t>( 1U ), timeout );
                    if( polled < xcbSuccess && errno == EINTR )
                    {
                        continue;
                    }
                    if( polled < xcbSuccess )
                    {
                        xcb_discard_reply( connection_.get(), cookie.sequence );
                        return std::unexpected( make_error(
                            ErrorCode::DeviceInaccessible,
                            system_error_message( "poll X11 overlay flush fence" )
                        ) );
                    }
                }
            }

            [[nodiscard]]
            SpacePoint
            edit_position( std::int16_t root_x,
                           std::int16_t root_y ) const noexcept
            {
                return SpacePoint{
                    .x     = static_cast<double>( root_x ),
                    .y     = static_cast<double>( root_y ),
                    .space = space_,
                };
            }

            static void
            invoke_edit_handler( const spi::OverlayEditHandler& handler,
                                 const spi::OverlayEditEvent&   event ) noexcept
            {
                try
                {
                    if( handler )
                    {
                        handler( event );
                    }
                }
                catch( ... )
                {
                    // An edit callback is isolated from the shared reactor loop.
                    return;
                }
            }

            void
            dispatch_edit_event( spi::OverlayEditEvent event,
                                 bool                  defer ) noexcept
            {
                spi::OverlayEditHandler handler;
                try
                {
                    handler = edit_handler_;
                }
                catch( ... )
                {
                    return;
                }
                if( !handler )
                {
                    return;
                }
                if( defer && reactor_ != nullptr )
                {
                    try
                    {
                        reactor_->post(
                            [handler = std::move( handler ), event]
                            {
                                invoke_edit_handler( handler, event );
                            }
                        );
                    }
                    catch( ... )
                    {
                        // Never dispatch inline while the service's flush lock may
                        // be held. Teardown will release any surviving pointer grab.
                        return;
                    }
                    return;
                }
                invoke_edit_handler( handler, event );
            }

            void
            notify_edit_surface_invalidated( bool defer ) noexcept
            {
                dispatch_edit_event(
                    spi::OverlayEditEvent{
                        .kind     = spi::OverlayEditEventKind::NotifyUngrab,
                        .position = SpacePoint{ .space = space_ },
                        .button   = {},
                    },
                    defer
                );
            }

            // Records that an input event with this X server timestamp is about
            // to drive the next frame, calibrating the server clock on first
            // sight. Only events delivered to the overlay window arrive here —
            // the trail path observes XI raw events elsewhere and does not pass
            // through this delegate, so `input_to_present` covers interactive
            // overlay input rather than every source.
            void
            note_input_timestamp( std::uint32_t server_ms ) noexcept
            {
                if( !server_clock_.calibrated() )
                {
                    server_clock_.calibrate( server_ms );
                }
                frame_input_server_ms_ = server_ms;
                ++frame_events_drained_;
            }

            void
            drain_events( bool defer_edit_events )
            {
                while( const auto event =
                           take_xcb_owned( xcb_poll_for_event( connection_.get() ) ) )
                {
                    const auto type = static_cast<std::uint8_t>( event->response_type &
                                                                 responseTypeMask );
                    if( type == 0U )
                    {
                        mark_async_desynced(
                            make_error( ErrorCode::ProtocolError,
                                        "X11 overlay received a protocol error" )
                        );
                        return;
                    }
                    if( type == XCB_BUTTON_PRESS || type == XCB_BUTTON_RELEASE )
                    {
                        const auto* button =
                            event_as<xcb_button_press_event_t>( event.get() );
                        if( button->event == window_ )
                        {
                            note_input_timestamp( button->time );
                            dispatch_edit_event(
                                spi::OverlayEditEvent{
                                    .kind = type == XCB_BUTTON_PRESS
                                              ? spi::OverlayEditEventKind::ButtonPress
                                              : spi::OverlayEditEventKind::ButtonRelease,
                                    .position =
                                        edit_position( button->root_x, button->root_y ),
                                    .button = button->detail,
                                },
                                defer_edit_events
                            );
                        }
                        continue;
                    }
                    if( type == XCB_MOTION_NOTIFY )
                    {
                        const auto* motion =
                            event_as<xcb_motion_notify_event_t>( event.get() );
                        if( motion->event == window_ )
                        {
                            note_input_timestamp( motion->time );
                            dispatch_edit_event(
                                spi::OverlayEditEvent{
                                    .kind = spi::OverlayEditEventKind::PointerMotion,
                                    .position =
                                        edit_position( motion->root_x, motion->root_y ),
                                    .button = {},
                                },
                                defer_edit_events
                            );
                        }
                        continue;
                    }
                    if( type == XCB_ENTER_NOTIFY || type == XCB_LEAVE_NOTIFY )
                    {
                        const auto* crossing =
                            event_as<xcb_enter_notify_event_t>( event.get() );
                        if( crossing->event ==
                            window_ &&
                            crossing->mode ==
                            XCB_NOTIFY_MODE_UNGRAB &&
                            pointer_grabbed_ )
                        {
                            pointer_grabbed_ = false;
                            dispatch_edit_event(
                                spi::OverlayEditEvent{
                                    .kind     = spi::OverlayEditEventKind::NotifyUngrab,
                                    .position = edit_position( crossing->root_x,
                                                               crossing->root_y ),
                                    .button   = {},
                                },
                                defer_edit_events
                            );
                        }
                        continue;
                    }
                    if( type ==
                        static_cast<std::uint8_t>( probe_.extensions.xfixes_first_event +
                                                   XCB_XFIXES_SELECTION_NOTIFY ) )
                    {
                        const auto* selection =
                            event_as<xcb_xfixes_selection_notify_event_t>( event.get() );
                        if( selection->selection == probe_.compositor_selection )
                        {
                            handle_compositor_owner( selection->owner,
                                                     defer_edit_events );
                        }
                        continue;
                    }
                    if( probe_.extensions.randr_available &&
                        type ==
                        static_cast<std::uint8_t>( probe_.extensions.randr_first_event +
                                                   XCB_RANDR_SCREEN_CHANGE_NOTIFY ) )
                    {
                        const auto* screen_change =
                            event_as<xcb_randr_screen_change_notify_event_t>(
                                event.get()
                            );
                        handle_topology_change( screen_change->width,
                                                screen_change->height,
                                                defer_edit_events );
                        continue;
                    }
                    if( type == XCB_CONFIGURE_NOTIFY )
                    {
                        const auto* configured =
                            event_as<xcb_configure_notify_event_t>( event.get() );
                        if( configured->window != window_ && mapped_ )
                        {
                            restack_best_effort();
                        }
                        continue;
                    }
                    if( type == XCB_MAP_NOTIFY )
                    {
                        const auto* mapped =
                            event_as<xcb_map_notify_event_t>( event.get() );
                        if( mapped->window != window_ && mapped_ )
                        {
                            restack_best_effort();
                        }
                    }
                }
                if( xcb_connection_has_error( connection_.get() ) != xcbSuccess )
                {
                    mark_async_desynced( make_error( ErrorCode::DeviceInaccessible,
                                                     "X11 overlay connection failed" ) );
                }
            }

            void
            handle_compositor_owner( xcb_window_t owner,
                                     bool         defer_edit_events )
            {
                probe_.compositor_owner = owner;
                if( owner != XCB_WINDOW_NONE )
                {
                    if( compositor_lost_ && should_map_ )
                    {
                        const bool grab_invalidated = pointer_grabbed_;
                        destroy_surface();
                        auto recreated = create_surface();
                        if( !recreated.has_value() )
                        {
                            destroy_surface();
                            mark_async_desynced( std::move( recreated.error() ) );
                            notify_availability( false );
                            if( grab_invalidated )
                            {
                                notify_edit_surface_invalidated( defer_edit_events );
                            }
                            return;
                        }
                        auto mapped = map_surface();
                        if( !mapped.has_value() )
                        {
                            destroy_surface();
                            mark_async_desynced( std::move( mapped.error() ) );
                            notify_availability( false );
                            if( grab_invalidated )
                            {
                                notify_edit_surface_invalidated( defer_edit_events );
                            }
                            return;
                        }
                        state_ = DelegateState::Synced;
                        last_error_.reset();
                        compositor_lost_    = false;
                        dirty_              = true;
                        surface_presented_  = false;
                        presented_revision_ = {};
                        ensure_frame_deadline();
                        schedule_wakeup();
                        if( grab_invalidated )
                        {
                            notify_edit_surface_invalidated( defer_edit_events );
                        }
                    }
                    notify_availability( true );
                    return;
                }

                const bool grab_invalidated = pointer_grabbed_;
                release_pointer_best_effort();
                if( mapped_ )
                {
                    xcb_unmap_window( connection_.get(), window_ );
                    xcb_flush( connection_.get() );
                    mapped_ = false;
                }
                compositor_lost_ = true;
                state_           = DelegateState::Desynced;
                last_error_      = capability_error( compositorUnavailableReason );
                notify_availability( false );
                if( grab_invalidated )
                {
                    notify_edit_surface_invalidated( defer_edit_events );
                }
            }

            void
            handle_topology_change( std::uint16_t width,
                                    std::uint16_t height,
                                    bool          defer_edit_events )
            {
                if( state_ == DelegateState::Closed || width == 0U || height == 0U )
                {
                    return;
                }
                if( topology_refresh_ )
                {
                    auto refreshed = topology_refresh_();
                    if( !refreshed.has_value() )
                    {
                        mark_async_desynced( std::move( refreshed.error() ) );
                        return;
                    }
                }

                probe_.screen.width  = width;
                probe_.screen.height = height;
                input_region_.clear();
                destroy_surface();
                auto created = create_surface();
                if( !created.has_value() )
                {
                    mark_async_desynced( std::move( created.error() ) );
                    notify_edit_surface_invalidated( defer_edit_events );
                    return;
                }
                if( should_map_ )
                {
                    auto owner = selection_owner( connection_.get(),
                                                  probe_.compositor_selection );
                    if( !owner.has_value() )
                    {
                        mark_async_desynced( std::move( owner.error() ) );
                        notify_edit_surface_invalidated( defer_edit_events );
                        return;
                    }
                    if( *owner == XCB_WINDOW_NONE )
                    {
                        handle_compositor_owner( XCB_WINDOW_NONE, defer_edit_events );
                        notify_edit_surface_invalidated( defer_edit_events );
                        return;
                    }
                    auto mapped = map_surface();
                    if( !mapped.has_value() )
                    {
                        mark_async_desynced( std::move( mapped.error() ) );
                        notify_edit_surface_invalidated( defer_edit_events );
                        return;
                    }
                }
                // The cached records were transformed under the OLD topology;
                // replaying them would freeze stale device coordinates. The
                // fresh surface starts empty and the delegate desynchronizes:
                // the owning service's next verb/flush resyncs with geometry
                // retransformed through the refreshed space graph.
                if( epoch_.has_value() )
                {
                    mark_async_desynced(
                        make_error( ErrorCode::ResyncRequired,
                                    "X11 overlay topology changed; service "
                                    "resync with retransformed geometry "
                                    "required" )
                    );
                }
                else
                {
                    dirty_ = true;
                    ensure_frame_deadline();
                    schedule_wakeup();
                }
                surface_presented_  = false;
                presented_revision_ = {};
                // Topology invalidates the cached native ShapeInput geometry.
                // The service refreshes the replacement window's region before
                // interpreting NotifyUngrab, and cancels an active drag if needed.
                notify_edit_surface_invalidated( defer_edit_events );
            }

            void
            restack_best_effort()
            {
                auto raised = raise_surface();
                if( !raised.has_value() )
                {
                    mark_async_desynced( std::move( raised.error() ) );
                    return;
                }
                xcb_flush( connection_.get() );
            }

            [[nodiscard]]
            Result<void>
            resync_required() const
            {
                if( last_error_.has_value() )
                {
                    return std::unexpected( *last_error_ );
                }
                return fail( ErrorCode::ResyncRequired,
                             "X11 overlay delegate requires resync" );
            }

            [[nodiscard]]
            Result<void>
            mark_desynced( std::string message )
            {
                return mark_desynced( make_error( ErrorCode::ResyncRequired,
                                                  std::move( message ) ) );
            }

            [[nodiscard]]
            Result<void>
            mark_desynced( Error error )
            {
                state_      = DelegateState::Desynced;
                last_error_ = error;
                return std::unexpected( std::move( error ) );
            }

            void
            mark_async_desynced( Error error )
            {
                state_      = DelegateState::Desynced;
                last_error_ = std::move( error );
            }

            void
            notify_availability( bool available )
            {
                if( availability_changed_ )
                {
                    const Error* const error =
                        !available && last_error_.has_value() ? &*last_error_ : nullptr;
                    availability_changed_( available, error );
                }
            }

            void
            close_without_notification() noexcept
            {
                ++timer_serial_;
                scheduled_wakeup_.reset();
                remove_event_monitor();
                destroy_surface();
                state_ = DelegateState::Closed;
                space_ = {};
                epoch_.reset();
                accepted_revision_  = {};
                presented_revision_ = {};
                shapes_.clear();
                input_region_.clear();
                edit_handler_       = {};
                dirty_              = false;
                should_map_         = false;
                has_frame_deadline_ = false;
                compositor_lost_    = false;
                pointer_grabbed_    = false;
                last_error_.reset();
            }

            OwnedConnection                    connection_;
            int                                screen_index_{};
            ProbeData                          probe_;
            core::Reactor*                     reactor_{};
            kernel::scheduling::PacingGovernor governor_;
            DelegateState                      state_{ DelegateState::Closed };
            CoordinateSpaceId                  space_{};
            std::optional<overlay::SceneEpoch> epoch_;
            overlay::Revision                  accepted_revision_{};
            overlay::Revision                  presented_revision_{};
            std::vector<overlay::ShapeRecord>  shapes_;
            std::vector<xcb_rectangle_t>       input_region_;
            std::optional<kernel::presentation::OverlayRaster>   raster_;
            xcb_colormap_t                                       colormap_{};
            xcb_window_t                                         window_{};
            xcb_gcontext_t                                       gc_{};
            std::size_t                                          native_stride_{};
            std::vector<std::byte>                               native_pixels_;
            ShmStorage                                           shm_;
            std::optional<std::uint64_t>                         event_token_;
            std::uint64_t                                        timer_serial_{};
            std::optional<std::chrono::steady_clock::time_point> scheduled_wakeup_;
            std::chrono::steady_clock::time_point                next_frame_deadline_;

            // Frame instrumentation. server_clock_ ties the X server's 32-bit
            // millisecond timestamps to steady_clock; without it an event
            // timestamp and a steady_clock reading share no origin and
            // subtracting them yields a meaningless latency.
            diag::ServerClock                                    server_clock_;
            std::optional<std::uint32_t>                         frame_input_server_ms_;
            std::uint32_t                                        frame_events_drained_{};

            std::optional<Error>                                 last_error_;
            AvailabilityChanged                                  availability_changed_;
            TopologyRefresh                                      topology_refresh_;
            spi::OverlayEditHandler                              edit_handler_;
            bool                                                 mapped_{};
            bool                                                 should_map_{};
            bool                                                 dirty_{};
            bool                                                 surface_presented_{};
            bool                                                 has_frame_deadline_{};
            bool                                                 compositor_lost_{};
            bool                                                 pointer_grabbed_{};
            bool native_memcpy_eligible_{};
    };

    X11OverlayDelegate::X11OverlayDelegate( std::shared_ptr<Impl> impl ) noexcept :
        impl_{ std::move( impl ) }
    {
    }

    X11OverlayDelegate::~X11OverlayDelegate() = default;

    Result<std::unique_ptr<X11OverlayDelegate>>
    X11OverlayDelegate::create( core::Reactor*   reactor,
                                std::string_view display )
    {
        auto connected = connect_display( display );
        if( !connected.has_value() )
        {
            return std::unexpected( std::move( connected.error() ) );
        }
        auto governor =
            kernel::scheduling::PacingGovernor::for_fps( targetFramesPerSecond );
        if( !governor.has_value() )
        {
            return std::unexpected( std::move( governor.error() ) );
        }
        auto impl = std::make_shared<Impl>( std::move( connected->connection ),
                                            connected->screen_index,
                                            reactor,
                                            *governor );
        return std::unique_ptr<X11OverlayDelegate>(
            new X11OverlayDelegate{ std::move( impl ) }
        );
    }

    Result<void>
    X11OverlayDelegate::probe( std::string_view display )
    {
        auto connected = connect_display( display );
        if( !connected.has_value() )
        {
            return std::unexpected( std::move( connected.error() ) );
        }
        auto result = probe_connection( connected->connection.get(),
                                        connected->screen_index,
                                        true );
        if( !result.has_value() )
        {
            return std::unexpected( std::move( result.error() ) );
        }
        return {};
    }

    Result<void>
    X11OverlayDelegate::open( CoordinateSpaceId space )
    {
        return impl_->open( space, true, true );
    }

    Result<void>
    X11OverlayDelegate::apply( std::span<const overlay::SceneDelta> deltas )
    {
        return impl_->apply( deltas );
    }

    Result<void>
    X11OverlayDelegate::resync( const overlay::SceneSnapshot& scene )
    {
        return impl_->resync( scene );
    }

    Result<void>
    X11OverlayDelegate::flush( overlay::Revision through )
    {
        return impl_->flush( through );
    }

    Result<void>
    X11OverlayDelegate::set_input_region(
        std::span<const geometry::Rectangle> rectangles
    )
    {
        return impl_->set_input_region( rectangles );
    }

    Result<void>
    X11OverlayDelegate::set_edit_handler( spi::OverlayEditHandler handler )
    {
        return impl_->set_edit_handler( std::move( handler ) );
    }

    geometry::Rectangle
    X11OverlayDelegate::surface_bounds() const noexcept
    {
        return impl_->surface_bounds();
    }

    Result<void>
    X11OverlayDelegate::grab_pointer()
    {
        return impl_->grab_pointer();
    }

    Result<void>
    X11OverlayDelegate::ungrab_pointer()
    {
        return impl_->ungrab_pointer();
    }

    void
    X11OverlayDelegate::close()
    {
        impl_->close();
    }

    void
    X11OverlayDelegate::set_availability_changed( AvailabilityChanged callback )
    {
        impl_->set_availability_changed( std::move( callback ) );
    }

    void
    X11OverlayDelegate::set_topology_refresh( TopologyRefresh callback )
    {
        impl_->set_topology_refresh( std::move( callback ) );
    }

    namespace detail
    {

        Result<void>
        X11OverlayDelegateTestAccess::open_unmapped( X11OverlayDelegate& delegate,
                                                     CoordinateSpaceId   space )
        {
            return delegate.impl_->open( space, false, false );
        }

        Result<void>
        X11OverlayDelegateTestAccess::open_mapped_without_compositor(
            X11OverlayDelegate& delegate,
            CoordinateSpaceId   space
        )
        {
            return delegate.impl_->open( space, false, true );
        }

        xcb_window_t
        X11OverlayDelegateTestAccess::window(
            const X11OverlayDelegate& delegate
        ) noexcept
        {
            return delegate.impl_->window();
        }

        void
        X11OverlayDelegateTestAccess::simulate_topology_change(
            X11OverlayDelegate& delegate,
            std::uint16_t       width,
            std::uint16_t       height
        )
        {
            delegate.impl_->simulate_topology_change( width, height );
        }

    }    // namespace detail

}    // namespace grab::drivers::desktop::x11
