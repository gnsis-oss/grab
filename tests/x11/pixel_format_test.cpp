#include "grab/image.hpp"
#include "grab/result.hpp"
#include "platform/x11/pixel_format.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstdint>
#include <xcb/xproto.h>
// clang-format on

namespace
{

    constexpr std::uint8_t  depth24          = 24U;
    constexpr std::uint8_t  depth16          = 16U;
    constexpr std::uint8_t  padded32_bpp     = 32U;
    constexpr std::uint8_t  packed24_bpp     = 24U;
    constexpr std::uint8_t  rgb565_bpp       = 16U;
    constexpr std::uint32_t lsb_first_order  = XCB_IMAGE_ORDER_LSB_FIRST;
    constexpr std::uint32_t msb_first_order  = XCB_IMAGE_ORDER_MSB_FIRST;
    constexpr auto          invalid_argument = grab::ErrorCode::InvalidArgument;
    constexpr auto          expected_format  = grab::PixelFormat::Bgr0;

}    // namespace

TEST( X11PixelFormat,
      Depth24Bpp32LittleEndianMapsToBgr0 )
{
    const auto format =
        grab::platform::x11::pixel_format_for( depth24, padded32_bpp, lsb_first_order );
    ASSERT_TRUE( format.has_value() ) << format.error().message;
    EXPECT_EQ( *format, expected_format );
}

TEST( X11PixelFormat,
      Depth24PackedBpp24IsRejected )
{
    const auto format =
        grab::platform::x11::pixel_format_for( depth24, packed24_bpp, lsb_first_order );
    ASSERT_FALSE( format.has_value() );
    EXPECT_EQ( format.error().code, invalid_argument );
}

TEST( X11PixelFormat,
      UnsupportedCombinationsAreRejected )
{
    const auto rgb565 =
        grab::platform::x11::pixel_format_for( depth16, rgb565_bpp, lsb_first_order );
    const auto big_endian =
        grab::platform::x11::pixel_format_for( depth24, padded32_bpp, msb_first_order );

    ASSERT_FALSE( rgb565.has_value() );
    EXPECT_EQ( rgb565.error().code, invalid_argument );
    ASSERT_FALSE( big_endian.has_value() );
    EXPECT_EQ( big_endian.error().code, invalid_argument );
}
