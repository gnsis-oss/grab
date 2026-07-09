#include "core/checked.hpp"
#include "core/pixel_traits.hpp"
#include "grab/geometry/rectangle.hpp"
#include "grab/image.hpp"
#include "grab/result.hpp"
#include "platform/x11/cursor_overlay.hpp"
#include "platform/x11/xcb_reply.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <xcb/xcb.h>
#include <xcb/xfixes.h>
#include <xcb/xproto.h>

namespace grab::platform::x11
{

    namespace
    {

        constexpr std::uint32_t byte_mask         = 0XFFU;
        constexpr std::uint32_t green_shift       = 8U;
        constexpr std::uint32_t red_shift         = 16U;
        constexpr std::uint32_t alpha_shift       = 24U;
        constexpr std::uint32_t max_channel_value = 255U;
        constexpr std::uint32_t alpha_rounding    = 127U;
        constexpr std::uint8_t  transparent       = 0U;
        constexpr auto          opaque = static_cast<std::uint8_t>( max_channel_value );
        constexpr std::uint32_t empty_dimension = 0U;
        constexpr std::size_t   single_element  = 1U;
        constexpr std::size_t   last_row_offset = 1U;

        struct CursorClip
        {
                std::size_t width    = 0U;
                std::size_t height   = 0U;
                std::size_t cursor_x = 0U;
                std::size_t cursor_y = 0U;
                std::size_t frame_x  = 0U;
                std::size_t frame_y  = 0U;
        };

        struct DecodedPixel
        {
                std::uint8_t blue  = 0U;
                std::uint8_t green = 0U;
                std::uint8_t red   = 0U;
                std::uint8_t alpha = 0U;
        };

        [[nodiscard]]
        std::optional<CursorClip>
        clip_cursor( const grab::geometry::Rectangle& region,
                     const CursorImage&               cursor )
        {
            const auto cursor_left = static_cast<std::int64_t>( cursor.hotspot_x ) -
                                     static_cast<std::int64_t>( cursor.xhot );
            const auto cursor_top  = static_cast<std::int64_t>( cursor.hotspot_y ) -
                                     static_cast<std::int64_t>( cursor.yhot );
            const auto cursor_right =
                cursor_left + static_cast<std::int64_t>( cursor.width );
            const auto cursor_bottom =
                cursor_top + static_cast<std::int64_t>( cursor.height );

            const auto region_left = static_cast<std::int64_t>( region.x );
            const auto region_top  = static_cast<std::int64_t>( region.y );
            const auto region_right =
                region_left + static_cast<std::int64_t>( region.width );
            const auto region_bottom =
                region_top + static_cast<std::int64_t>( region.height );

            const std::int64_t clip_left   = std::max( cursor_left, region_left );
            const std::int64_t clip_top    = std::max( cursor_top, region_top );
            const std::int64_t clip_right  = std::min( cursor_right, region_right );
            const std::int64_t clip_bottom = std::min( cursor_bottom, region_bottom );

            if( clip_right <= clip_left || clip_bottom <= clip_top )
            {
                return std::nullopt;
            }

            return CursorClip{
                .width    = static_cast<std::size_t>( clip_right - clip_left ),
                .height   = static_cast<std::size_t>( clip_bottom - clip_top ),
                .cursor_x = static_cast<std::size_t>( clip_left - cursor_left ),
                .cursor_y = static_cast<std::size_t>( clip_top - cursor_top ),
                .frame_x  = static_cast<std::size_t>( clip_left - region_left ),
                .frame_y  = static_cast<std::size_t>( clip_top - region_top ),
            };
        }

        [[nodiscard]]
        bool
        cursor_pixels_available( const CursorImage& cursor )
        {
            const auto pixel_count = grab::checked_mul<std::size_t>(
                static_cast<std::size_t>( cursor.width ),
                static_cast<std::size_t>( cursor.height ),
                grab::ErrorCode::provider_failed,
                "XFixes cursor image is too large"
            );
            return pixel_count.has_value() && cursor.pixels.size() >= *pixel_count;
        }

        template<grab::PixelFormat Format>
        [[nodiscard]]
        bool
        frame_bytes_available( std::span<const std::uint8_t>    frame_bgr0,
                               std::size_t                      stride,
                               const grab::geometry::Rectangle& region )
        {
            using Traits = grab::PixelTraits<Format>;

            if( region.width == empty_dimension || region.height == empty_dimension )
            {
                return false;
            }

            const auto row_bytes =
                grab::checked_mul<std::size_t>( static_cast<std::size_t>( region.width ),
                                                Traits::bytes_per_pixel,
                                                grab::ErrorCode::provider_failed,
                                                "cursor frame row is too large" );
            if( !row_bytes.has_value() || stride < *row_bytes )
            {
                return false;
            }

            const auto tail_rows =
                static_cast<std::size_t>( region.height - last_row_offset );
            const auto tail_bytes =
                grab::checked_mul<std::size_t>( tail_rows,
                                                stride,
                                                grab::ErrorCode::provider_failed,
                                                "cursor frame is too large" );
            if( !tail_bytes.has_value() )
            {
                return false;
            }
            const auto required_bytes =
                grab::checked_add<std::size_t>( *tail_bytes,
                                                *row_bytes,
                                                grab::ErrorCode::provider_failed,
                                                "cursor frame is too large" );
            return required_bytes.has_value() && frame_bgr0.size() >= *required_bytes;
        }

        [[nodiscard]]
        DecodedPixel
        decode_cursor_pixel( std::uint32_t pixel ) noexcept
        {
            return DecodedPixel{
                .blue = static_cast<std::uint8_t>( pixel & byte_mask ),
                .green =
                    static_cast<std::uint8_t>( ( pixel >> green_shift ) & byte_mask ),
                .red = static_cast<std::uint8_t>( ( pixel >> red_shift ) & byte_mask ),
                .alpha =
                    static_cast<std::uint8_t>( ( pixel >> alpha_shift ) & byte_mask ),
            };
        }

        [[nodiscard]]
        std::uint8_t
        blend_channel( std::uint8_t premultiplied,
                       std::uint8_t background,
                       std::uint8_t alpha ) noexcept
        {
            const std::uint32_t premultiplied_wide = premultiplied;
            const std::uint32_t background_wide    = background;
            const std::uint32_t alpha_wide         = alpha;
            const std::uint32_t inverse_alpha      = max_channel_value - alpha_wide;
            const std::uint32_t background_part =
                ( ( background_wide * inverse_alpha ) + alpha_rounding ) /
                max_channel_value;
            const std::uint32_t blended = premultiplied_wide + background_part;
            return static_cast<std::uint8_t>( blended );
        }

        template<grab::PixelFormat Format>
        void
        apply_cursor_pixel( std::span<std::uint8_t> target,
                            DecodedPixel            pixel )
        {
            using Traits = grab::PixelTraits<Format>;

            if( pixel.alpha == transparent )
            {
                return;
            }

            if( pixel.alpha == opaque )
            {
                Traits::write_rgb( target, pixel.red, pixel.green, pixel.blue );
                return;
            }

            const std::uint8_t blue =
                blend_channel( pixel.blue, Traits::blue( target ), pixel.alpha );
            const std::uint8_t green =
                blend_channel( pixel.green, Traits::green( target ), pixel.alpha );
            const std::uint8_t red =
                blend_channel( pixel.red, Traits::red( target ), pixel.alpha );
            Traits::write_rgb( target, red, green, blue );
        }

        template<grab::PixelFormat Format>
        [[nodiscard]]
        std::size_t
        frame_offset( std::size_t stride,
                      std::size_t x,
                      std::size_t y ) noexcept
        {
            return ( y * stride ) + ( x * grab::PixelTraits<Format>::bytes_per_pixel );
        }

        [[nodiscard]]
        std::size_t
        cursor_offset( std::size_t cursor_width,
                       std::size_t x,
                       std::size_t y ) noexcept
        {
            return ( y * cursor_width ) + x;
        }

        [[nodiscard]]
        std::uint32_t
        cursor_pixel_at( std::span<const std::uint32_t> pixels,
                         std::size_t                    offset )
        {
            return pixels.subspan( offset, single_element ).front();
        }

        [[nodiscard]]
        bool
        xfixes_extension_present( xcb_connection_t* conn ) noexcept
        {
            const xcb_query_extension_reply_t* extension =
                xcb_get_extension_data( conn, &xcb_xfixes_id );
            return extension != nullptr && extension->present != 0;
        }

        [[nodiscard]]
        grab::Result<std::size_t>
        cursor_pixel_count( const xcb_xfixes_get_cursor_image_reply_t& reply )
        {
            return grab::checked_mul<std::size_t>(
                static_cast<std::size_t>( reply.width ),
                static_cast<std::size_t>( reply.height ),
                grab::ErrorCode::provider_failed,
                "XFixes cursor image is too large"
            );
        }

        template<grab::PixelFormat Format>
        void
        composite_cursor_kernel( std::span<std::uint8_t>          frame_bgr0,
                                 std::size_t                      stride,
                                 const grab::geometry::Rectangle& region,
                                 const CursorImage&               cursor )
        {
            using Traits    = grab::PixelTraits<Format>;

            const auto clip = clip_cursor( region, cursor );
            if( !clip.has_value() )
            {
                return;
            }
            if( !cursor_pixels_available( cursor ) ||
                !frame_bytes_available<Format>( frame_bgr0, stride, region ) )
            {
                return;
            }

            const auto cursor_width = static_cast<std::size_t>( cursor.width );
            for( std::size_t row = 0U; row < clip->height; ++row )
            {
                const std::size_t cursor_y = clip->cursor_y + row;
                const std::size_t frame_y  = clip->frame_y + row;
                for( std::size_t col = 0U; col < clip->width; ++col )
                {
                    const std::size_t   cursor_x = clip->cursor_x + col;
                    const std::size_t   frame_x  = clip->frame_x + col;
                    const std::uint32_t pixel    = cursor_pixel_at(
                        cursor.pixels,
                        cursor_offset( cursor_width, cursor_x, cursor_y )
                    );
                    const std::size_t target_offset =
                        frame_offset<Format>( stride, frame_x, frame_y );
                    apply_cursor_pixel<Format>(
                        frame_bgr0.subspan( target_offset, Traits::bytes_per_pixel ),
                        decode_cursor_pixel( pixel )
                    );
                }
            }
        }

        using CursorComposite = void ( * )( std::span<std::uint8_t>,
                                            std::size_t,
                                            const grab::geometry::Rectangle&,
                                            const CursorImage& );

        struct CursorDispatch
        {
                grab::PixelFormat format    = grab::PixelFormat::bgr0;
                CursorComposite   composite = nullptr;
        };

        constexpr std::size_t cursor_dispatch_count = 1U;
        constexpr std::array<CursorDispatch, cursor_dispatch_count> cursor_dispatch{
            CursorDispatch{
                           .format    = grab::PixelFormat::bgr0,
                           .composite = &composite_cursor_kernel<grab::PixelFormat::bgr0>,
                           },
        };

        void
        composite_cursor_for_format( grab::PixelFormat                format,
                                     std::span<std::uint8_t>          frame_bgr0,
                                     std::size_t                      stride,
                                     const grab::geometry::Rectangle& region,
                                     const CursorImage&               cursor )
        {
            for( const CursorDispatch& dispatch : cursor_dispatch )
            {
                if( dispatch.format == format && dispatch.composite != nullptr )
                {
                    dispatch.composite( frame_bgr0, stride, region, cursor );
                    return;
                }
            }
        }

    }    // namespace

    void
    composite_cursor( std::span<std::uint8_t>          frame_bgr0,
                      std::size_t                      stride,
                      const grab::geometry::Rectangle& region,
                      const CursorImage&               cursor )
    {
        composite_cursor_for_format( grab::PixelFormat::bgr0,
                                     frame_bgr0,
                                     stride,
                                     region,
                                     cursor );
    }

    grab::Result<void>
    draw_xfixes_cursor( xcb_connection_t*                conn,
                        std::span<std::uint8_t>          frame_bgr0,
                        std::size_t                      stride,
                        const grab::geometry::Rectangle& region )
    {
        if( conn == nullptr || !xfixes_extension_present( conn ) )
        {
            return {};
        }

        const xcb_xfixes_get_cursor_image_cookie_t cookie =
            xcb_xfixes_get_cursor_image( conn );
        xcb_generic_error_t* error_raw = nullptr;
        auto                 reply     = make_xcb_reply(
            xcb_xfixes_get_cursor_image_reply( conn, cookie, &error_raw )
        );
        auto error = make_xcb_reply( error_raw );
        if( error != nullptr || reply == nullptr )
        {
            return {};
        }

        auto pixel_count = cursor_pixel_count( *reply );
        if( !pixel_count.has_value() )
        {
            return grab::fail( pixel_count.error().code, pixel_count.error().message );
        }

        const int length =
            xcb_xfixes_get_cursor_image_cursor_image_length( reply.get() );
        if( length < 0 || !std::in_range<std::size_t>( length ) )
        {
            return grab::fail( grab::ErrorCode::provider_failed,
                               "XFixes cursor image length is invalid" );
        }
        const auto actual_count = static_cast<std::size_t>( length );
        if( actual_count < *pixel_count )
        {
            return grab::fail( grab::ErrorCode::provider_failed,
                               "XFixes cursor image length is too short" );
        }

        const std::uint32_t* pixels =
            xcb_xfixes_get_cursor_image_cursor_image( reply.get() );
        if( pixels == nullptr && *pixel_count != 0U )
        {
            return grab::fail( grab::ErrorCode::provider_failed,
                               "XFixes cursor image data is null" );
        }

        const CursorImage cursor{
            .width     = reply->width,
            .height    = reply->height,
            .hotspot_x = reply->x,
            .hotspot_y = reply->y,
            .xhot      = reply->xhot,
            .yhot      = reply->yhot,
            .pixels    = std::span<const std::uint32_t>{ pixels, *pixel_count },
        };
        composite_cursor( frame_bgr0, stride, region, cursor );
        return {};
    }

}    // namespace grab::platform::x11
