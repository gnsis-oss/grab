#include "drivers/desktop/x11/overlay_delegate.hpp"
#include "grab/capability.hpp"
#include "grab/geometry/rectangle.hpp"
#include "grab/geometry/size.hpp"
#include "grab/image.hpp"
#include "grab/overlay.hpp"
#include "grab/result.hpp"
#include "grab/space.hpp"
#include "kernel/presentation/overlay_raster.hpp"
#include "kernel/scheduling/pacing_governor.hpp"
#include "kernel/scheduling/reactor.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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

        constexpr std::uint32_t    targetFramesPerSecond = 60U;
        constexpr std::uint8_t     argbDepth             = 32U;
        constexpr std::uint8_t     bitsPerByte           = 8U;
        constexpr std::uint8_t     bgraBytesPerPixel     = 4U;
        constexpr std::uint8_t     fullChannel           = 0XFFU;
        constexpr std::uint8_t     responseTypeMask      = 0X7FU;
        constexpr std::uint8_t     noEventPropagation    = 0U;
        constexpr std::uint8_t     noShmCompletionEvent  = 0U;
        constexpr std::uint32_t    noRectangles          = 0U;
        constexpr std::uint32_t    revisionStep          = 1U;
        constexpr int              xcbSuccess            = 0;
        constexpr int              flushFailure          = 0;
        constexpr int              invalidFileDescriptor = -1;
        constexpr int              systemCallFailure     = -1;
        constexpr int              noPollEvents          = 0;
        constexpr std::int16_t     xcbPollEvents         = POLLIN;
        constexpr int              shmPermissions        = 0600;
        constexpr auto             noTimerDelay      = std::chrono::nanoseconds::zero();
        constexpr auto             flushFenceTimeout = std::chrono::seconds{ 2 };
        constexpr std::string_view argbUnavailableReason{
            "X11 overlay requires an XRender ARGB32 visual"
        };
        constexpr std::string_view xfixesUnavailableReason{
            "X11 overlay requires XFixes ShapeInput support"
        };
        constexpr std::string_view compositorUnavailableReason{
            "X11 overlay requires an owned compositing manager selection"
        };
        constexpr std::string_view compositorSelectionPrefix{ "_NET_WM_CM_S" };

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
        Result<ExtensionSpec>
        probe_extensions( xcb_connection_t* connection )
        {
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

            ExtensionSpec result{
                .xfixes_first_event = xfixes->first_event,
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
            return ProbeData{
                .screen               = *screen,
                .layout               = *layout,
                .extensions           = *extensions,
                .compositor_selection = *atom,
                .compositor_owner     = *owner,
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
                .render_frame           = scene_dirty,
                .continue_fade          = false,
                .next_lifetime_deadline = std::nullopt,
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
            }
            return plan;
        }

        Result<void>
        apply_input_passthrough( xcb_connection_t* connection,
                                 xcb_window_t      window )
        {
            if( connection == nullptr || window == XCB_WINDOW_NONE )
            {
                return fail( ErrorCode::InvalidArgument,
                             "X11 overlay input passthrough requires a window" );
            }
            return check_request(
                connection,
                xcb_shape_rectangles_checked( connection,
                                              XCB_SHAPE_SO_SET,
                                              XCB_SHAPE_SK_INPUT,
                                              XCB_CLIP_ORDERING_UNSORTED,
                                              window,
                                              0,
                                              0,
                                              noRectangles,
                                              nullptr ),
                "apply empty X11 overlay ShapeInput region"
            );
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
        convert_image( const Image&         source,
                       const NativeLayout&  layout,
                       std::size_t          destination_stride,
                       std::span<std::byte> destination )
        {
            if( source.format !=
                PixelFormat::Bgra ||
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
            const auto required =
                destination_stride * static_cast<std::size_t>( source.height );
            if( destination.size() < required )
            {
                return fail( ErrorCode::ProtocolError,
                             "X11 overlay native pixel storage is too short" );
            }

            std::ranges::fill( destination.first( required ), std::byte{} );
            for( std::uint32_t y{}; y < source.height; ++y )
            {
                const auto source_row = source.row( y );
                if( source_row.size() <
                    static_cast<std::size_t>( source.width ) *
                    bgraBytesPerPixel )
                {
                    return fail( ErrorCode::ProtocolError,
                                 "X11 overlay raster row is too short" );
                }
                auto* const destination_row =
                    destination.data() +
                    ( static_cast<std::size_t>( y ) * destination_stride );
                for( std::uint32_t x{}; x < source.width; ++x )
                {
                    const auto source_offset =
                        static_cast<std::size_t>( x ) * bgraBytesPerPixel;
                    const auto blue =
                        std::to_integer<std::uint8_t>( source_row[source_offset] );
                    const auto green =
                        std::to_integer<std::uint8_t>( source_row[source_offset + 1U] );
                    const auto red =
                        std::to_integer<std::uint8_t>( source_row[source_offset + 2U] );
                    const auto alpha =
                        std::to_integer<std::uint8_t>( source_row[source_offset + 3U] );
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
                probe_       = *refreshed;

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
                    return std::unexpected( std::move( unavailable ) );
                }
                probe_.compositor_owner = *compositor;

                if( should_map_ && compositor_lost_ )
                {
                    destroy_surface();
                    auto recreated = create_surface();
                    if( !recreated.has_value() )
                    {
                        destroy_surface();
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
                        return mark_desynced( std::move( mapped.error() ) );
                    }
                }
                compositor_lost_ = false;
                notify_availability( true );
                ensure_frame_deadline();
                schedule_wakeup();
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
                drain_events();
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
                drain_events();
                if( state_ == DelegateState::Desynced )
                {
                    return resync_required();
                }
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
                handle_topology_change( width, height );
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
                    static_cast<std::uint32_t>( XCB_EVENT_MASK_EXPOSURE ),
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

                auto passthrough =
                    detail::apply_input_passthrough( connection_.get(), window_ );
                if( !passthrough.has_value() )
                {
                    return passthrough;
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
                release_shm();
                native_pixels_.clear();
                if( probe_.extensions.shm_available && try_allocate_shm( size ) )
                {
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
                mapped_            = false;
                native_stride_     = 0U;
                surface_presented_ = false;
                if( connection_ != nullptr )
                {
                    xcb_flush( connection_.get() );
                }
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
                                    impl->drain_events();
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
                drain_events();
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
                auto frame = raster_->render( shapes_, monotonic_milliseconds() );
                if( !frame.has_value() )
                {
                    return std::unexpected( std::move( frame.error() ) );
                }
                if( !frame->damage.empty() )
                {
                    auto converted = convert_image( frame->pixels,
                                                    probe_.layout,
                                                    native_stride_,
                                                    native_storage() );
                    if( !converted.has_value() )
                    {
                        return converted;
                    }
                    auto presented = shm_.attached ? present_shm( frame->damage )
                                                   : present_put_image( frame->damage );
                    if( !presented.has_value() )
                    {
                        return presented;
                    }
                }
                if( xcb_flush( connection_.get() ) <=
                    flushFailure ||
                    xcb_connection_has_error( connection_.get() ) != xcbSuccess )
                {
                    return fail( ErrorCode::DeviceInaccessible,
                                 "X11 overlay presentation flush failed" );
                }
                dirty_              = false;
                surface_presented_  = true;
                presented_revision_ = accepted_revision_;
                return {};
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

            void
            drain_events()
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
                    if( type ==
                        static_cast<std::uint8_t>( probe_.extensions.xfixes_first_event +
                                                   XCB_XFIXES_SELECTION_NOTIFY ) )
                    {
                        const auto* selection =
                            event_as<xcb_xfixes_selection_notify_event_t>( event.get() );
                        if( selection->selection == probe_.compositor_selection )
                        {
                            handle_compositor_owner( selection->owner );
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
                                                screen_change->height );
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
            handle_compositor_owner( xcb_window_t owner )
            {
                probe_.compositor_owner = owner;
                if( owner != XCB_WINDOW_NONE )
                {
                    if( compositor_lost_ && should_map_ )
                    {
                        destroy_surface();
                        auto recreated = create_surface();
                        if( !recreated.has_value() )
                        {
                            destroy_surface();
                            mark_async_desynced( std::move( recreated.error() ) );
                            notify_availability( false );
                            return;
                        }
                        auto mapped = map_surface();
                        if( !mapped.has_value() )
                        {
                            destroy_surface();
                            mark_async_desynced( std::move( mapped.error() ) );
                            notify_availability( false );
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
                    }
                    notify_availability( true );
                    return;
                }

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
            }

            void
            handle_topology_change( std::uint16_t width,
                                    std::uint16_t height )
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
                destroy_surface();
                auto created = create_surface();
                if( !created.has_value() )
                {
                    mark_async_desynced( std::move( created.error() ) );
                    return;
                }
                if( should_map_ )
                {
                    auto owner = selection_owner( connection_.get(),
                                                  probe_.compositor_selection );
                    if( !owner.has_value() )
                    {
                        mark_async_desynced( std::move( owner.error() ) );
                        return;
                    }
                    if( *owner == XCB_WINDOW_NONE )
                    {
                        handle_compositor_owner( XCB_WINDOW_NONE );
                        return;
                    }
                    auto mapped = map_surface();
                    if( !mapped.has_value() )
                    {
                        mark_async_desynced( std::move( mapped.error() ) );
                        return;
                    }
                }
                // Reapply the complete retained abstract scene to the newly
                // created raster. Its first render is full-surface damage.
                if( epoch_.has_value() )
                {
                    auto resynced = resync( overlay::SceneSnapshot{
                        .epoch            = *epoch_,
                        .through_revision = accepted_revision_,
                        .shapes           = shapes_,
                    } );
                    if( !resynced.has_value() )
                    {
                        mark_async_desynced( std::move( resynced.error() ) );
                        return;
                    }
                }
                else
                {
                    dirty_ = true;
                    ensure_frame_deadline();
                    schedule_wakeup();
                }
                surface_presented_  = false;
                presented_revision_ = {};
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
                dirty_              = false;
                should_map_         = false;
                has_frame_deadline_ = false;
                compositor_lost_    = false;
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
            std::optional<Error>                                 last_error_;
            AvailabilityChanged                                  availability_changed_;
            TopologyRefresh                                      topology_refresh_;
            bool                                                 mapped_{};
            bool                                                 should_map_{};
            bool                                                 dirty_{};
            bool                                                 surface_presented_{};
            bool                                                 has_frame_deadline_{};
            bool                                                 compositor_lost_{};
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
