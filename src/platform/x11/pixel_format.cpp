#include "grab/image.hpp"
#include "grab/result.hpp"
#include "platform/x11/pixel_format.hpp"

#include <cstdint>
#include <string>
#include <xcb/xproto.h>

namespace grab::platform::x11
{

    namespace
    {

        constexpr std::uint8_t  depth24         = 24U;
        constexpr std::uint8_t  packed24_bpp    = 24U;
        constexpr std::uint8_t  padded32_bpp    = 32U;
        constexpr std::uint32_t lsb_first_order = XCB_IMAGE_ORDER_LSB_FIRST;

        [[nodiscard]]
        std::string
        unsupported_message( std::uint8_t  depth,
                             std::uint8_t  bits_per_pixel,
                             std::uint32_t image_byte_order )
        {
            return "unsupported X pixmap format depth=" +
                   std::to_string( static_cast<unsigned int>( depth ) ) +
                   " bits_per_pixel=" +
                   std::to_string( static_cast<unsigned int>( bits_per_pixel ) ) +
                   " byte_order=" +
                   std::to_string( image_byte_order );
        }

    }    // namespace

    grab::Result<grab::PixelFormat>
    pixel_format_for( std::uint8_t  depth,
                      std::uint8_t  bits_per_pixel,
                      std::uint32_t image_byte_order )
    {
        if( depth ==
            depth24 &&
            bits_per_pixel ==
            padded32_bpp &&
            image_byte_order == lsb_first_order )
        {
            return grab::PixelFormat::bgr0;
        }

        if( depth ==
            depth24 &&
            bits_per_pixel ==
            packed24_bpp &&
            image_byte_order == lsb_first_order )
        {
            return grab::fail(
                grab::ErrorCode::invalid_argument,
                "24bpp packed capture unsupported; only 32bpp X pixmaps are "
                "supported for now"
            );
        }

        return grab::fail(
            grab::ErrorCode::invalid_argument,
            unsupported_message( depth, bits_per_pixel, image_byte_order )
        );
    }

}    // namespace grab::platform::x11
