#include "grab/geometry/rectangle.hpp"
#include "platform/x11/cursor_overlay.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
// clang-format on

namespace
{

    constexpr std::uint32_t green_shift       = 8U;
    constexpr std::uint32_t red_shift         = 16U;
    constexpr std::uint32_t alpha_shift       = 24U;
    constexpr std::uint32_t opaque_alpha_wide = 255U;
    constexpr auto         opaque_alpha = static_cast<std::uint8_t>( opaque_alpha_wide );
    constexpr std::uint8_t zero_byte    = 0U;
    constexpr std::size_t  bgr0_bytes_per_pixel = 4U;
    constexpr std::size_t  blue_offset          = 0U;
    constexpr std::size_t  green_offset         = 1U;
    constexpr std::size_t  red_offset           = 2U;
    constexpr std::size_t  padding_offset       = 3U;
    constexpr std::size_t  first_pixel_offset   = 0U;
    constexpr std::size_t  next_pixel_offset    = 1U;
    constexpr std::int32_t no_hotspot_offset    = 0;
    constexpr std::int32_t one_pixel_hotspot    = 1;
    constexpr std::int32_t zero_region_offset   = 0;
    constexpr std::uint8_t initial_blue         = 1U;
    constexpr std::uint8_t initial_green        = 2U;
    constexpr std::uint8_t initial_red          = 3U;
    constexpr std::uint8_t opaque_first_blue    = 10U;
    constexpr std::uint8_t opaque_first_green   = 20U;
    constexpr std::uint8_t opaque_first_red     = 30U;
    constexpr std::uint8_t opaque_second_blue   = 40U;
    constexpr std::uint8_t opaque_second_green  = 50U;
    constexpr std::uint8_t opaque_second_red    = 60U;
    constexpr std::uint8_t opaque_third_blue    = 70U;
    constexpr std::uint8_t opaque_third_green   = 80U;
    constexpr std::uint8_t opaque_third_red     = 90U;
    constexpr std::uint8_t opaque_fourth_blue   = 100U;
    constexpr std::uint8_t opaque_fourth_green  = 110U;
    constexpr std::uint8_t opaque_fourth_red    = 120U;

    [[nodiscard]]
    constexpr std::uint32_t
    u32_size( std::size_t value ) noexcept
    {
        return static_cast<std::uint32_t>( value );
    }

    [[nodiscard]]
    constexpr std::uint32_t
    cursor_pixel( std::uint8_t blue,
                  std::uint8_t green,
                  std::uint8_t red,
                  std::uint8_t alpha ) noexcept
    {
        return ( static_cast<std::uint32_t>( alpha ) << alpha_shift ) |
               ( static_cast<std::uint32_t>( red ) << red_shift ) |
               ( static_cast<std::uint32_t>( green ) << green_shift ) |
               static_cast<std::uint32_t>( blue );
    }

    [[nodiscard]]
    constexpr std::size_t
    pixel_offset( std::size_t stride,
                  std::size_t x,
                  std::size_t y ) noexcept
    {
        return ( y * stride ) + ( x * bgr0_bytes_per_pixel );
    }

    template<std::size_t ByteCount>
    void
    set_frame_pixel( std::array<std::uint8_t,
                                ByteCount>& frame,
                     std::size_t            stride,
                     std::size_t            x,
                     std::size_t            y,
                     std::uint8_t           blue,
                     std::uint8_t           green,
                     std::uint8_t           red )
    {
        const std::size_t offset            = pixel_offset( stride, x, y );
        frame.at( offset + blue_offset )    = blue;
        frame.at( offset + green_offset )   = green;
        frame.at( offset + red_offset )     = red;
        frame.at( offset + padding_offset ) = zero_byte;
    }

    template<std::size_t ByteCount>
    void
    fill_frame( std::array<std::uint8_t,
                           ByteCount>& frame,
                std::size_t            stride,
                std::size_t            width,
                std::size_t            height,
                std::uint8_t           blue,
                std::uint8_t           green,
                std::uint8_t           red )
    {
        for( std::size_t y = 0U; y < height; ++y )
        {
            for( std::size_t x = 0U; x < width; ++x )
            {
                set_frame_pixel( frame, stride, x, y, blue, green, red );
            }
        }
    }

    template<std::size_t ByteCount>
    void
    expect_frame_pixel( const std::array<std::uint8_t,
                                         ByteCount>& frame,
                        std::size_t                  stride,
                        std::size_t                  x,
                        std::size_t                  y,
                        std::uint8_t                 blue,
                        std::uint8_t                 green,
                        std::uint8_t                 red )
    {
        const std::size_t offset = pixel_offset( stride, x, y );
        EXPECT_EQ( frame.at( offset + blue_offset ), blue );
        EXPECT_EQ( frame.at( offset + green_offset ), green );
        EXPECT_EQ( frame.at( offset + red_offset ), red );
        EXPECT_EQ( frame.at( offset + padding_offset ), zero_byte );
    }

}    // namespace

TEST( X11CursorOverlay,
      OpaqueCursorOverwritesBgrAtOffset )
{
    constexpr std::int32_t region_x           = 10;
    constexpr std::int32_t region_y           = 20;
    constexpr std::size_t  frame_width        = 4U;
    constexpr std::size_t  frame_height       = 4U;
    constexpr std::size_t  frame_stride       = frame_width * bgr0_bytes_per_pixel;
    constexpr std::size_t  frame_byte_count   = frame_stride * frame_height;
    constexpr std::size_t  cursor_width       = 2U;
    constexpr std::size_t  cursor_height      = 2U;
    constexpr std::size_t  cursor_pixel_count = cursor_width * cursor_height;
    constexpr std::int32_t cursor_hotspot_x   = 12;
    constexpr std::int32_t cursor_hotspot_y   = 22;
    constexpr std::size_t  frame_target_x     = 1U;
    constexpr std::size_t  frame_target_y     = 1U;

    std::array<std::uint8_t, frame_byte_count> frame{};
    fill_frame( frame,
                frame_stride,
                frame_width,
                frame_height,
                initial_blue,
                initial_green,
                initial_red );

    constexpr std::array<std::uint32_t, cursor_pixel_count> pixels{
        cursor_pixel( opaque_first_blue,
                      opaque_first_green,
                      opaque_first_red,
                      opaque_alpha ),
        cursor_pixel( opaque_second_blue,
                      opaque_second_green,
                      opaque_second_red,
                      opaque_alpha ),
        cursor_pixel( opaque_third_blue,
                      opaque_third_green,
                      opaque_third_red,
                      opaque_alpha ),
        cursor_pixel( opaque_fourth_blue,
                      opaque_fourth_green,
                      opaque_fourth_red,
                      opaque_alpha ),
    };

    const grab::geometry::Rectangle region{
        .x      = region_x,
        .y      = region_y,
        .width  = u32_size( frame_width ),
        .height = u32_size( frame_height ),
    };
    const grab::platform::x11::CursorImage cursor{
        .width     = u32_size( cursor_width ),
        .height    = u32_size( cursor_height ),
        .hotspot_x = cursor_hotspot_x,
        .hotspot_y = cursor_hotspot_y,
        .xhot      = one_pixel_hotspot,
        .yhot      = one_pixel_hotspot,
        .pixels    = std::span<const std::uint32_t>{ pixels },
    };

    grab::platform::x11::composite_cursor( std::span<std::uint8_t>{ frame },
                                           frame_stride,
                                           region,
                                           cursor );

    expect_frame_pixel( frame,
                        frame_stride,
                        frame_target_x,
                        frame_target_y,
                        opaque_first_blue,
                        opaque_first_green,
                        opaque_first_red );
    expect_frame_pixel( frame,
                        frame_stride,
                        frame_target_x + next_pixel_offset,
                        frame_target_y,
                        opaque_second_blue,
                        opaque_second_green,
                        opaque_second_red );
    expect_frame_pixel( frame,
                        frame_stride,
                        frame_target_x,
                        frame_target_y + next_pixel_offset,
                        opaque_third_blue,
                        opaque_third_green,
                        opaque_third_red );
    expect_frame_pixel( frame,
                        frame_stride,
                        frame_target_x + next_pixel_offset,
                        frame_target_y + next_pixel_offset,
                        opaque_fourth_blue,
                        opaque_fourth_green,
                        opaque_fourth_red );
    expect_frame_pixel( frame,
                        frame_stride,
                        first_pixel_offset,
                        first_pixel_offset,
                        initial_blue,
                        initial_green,
                        initial_red );
}

TEST( X11CursorOverlay,
      SemiTransparentPixelUsesRoundedPremultipliedOver )
{
    constexpr std::size_t  frame_width        = 1U;
    constexpr std::size_t  frame_height       = 1U;
    constexpr std::size_t  frame_stride       = frame_width * bgr0_bytes_per_pixel;
    constexpr std::size_t  frame_byte_count   = frame_stride * frame_height;
    constexpr std::size_t  cursor_width       = 1U;
    constexpr std::size_t  cursor_height      = 1U;
    constexpr std::size_t  cursor_pixel_count = cursor_width * cursor_height;
    constexpr std::uint8_t background_blue    = 100U;
    constexpr std::uint8_t background_green   = 110U;
    constexpr std::uint8_t background_red     = 120U;
    constexpr std::uint8_t cursor_blue        = 20U;
    constexpr std::uint8_t cursor_green       = 40U;
    constexpr std::uint8_t cursor_red         = 60U;
    constexpr std::uint8_t cursor_alpha       = 128U;
    constexpr std::uint8_t expected_blue      = 70U;
    constexpr std::uint8_t expected_green     = 95U;
    constexpr std::uint8_t expected_red       = 120U;
    constexpr std::int32_t cursor_hotspot_x   = 0;
    constexpr std::int32_t cursor_hotspot_y   = 0;
    constexpr std::size_t  frame_target_x     = 0U;
    constexpr std::size_t  frame_target_y     = 0U;

    std::array<std::uint8_t, frame_byte_count> frame{};
    fill_frame( frame,
                frame_stride,
                frame_width,
                frame_height,
                background_blue,
                background_green,
                background_red );

    std::array<std::uint32_t, cursor_pixel_count> pixels{};
    pixels.fill( cursor_pixel( cursor_blue, cursor_green, cursor_red, cursor_alpha ) );
    const grab::geometry::Rectangle region{
        .x      = zero_region_offset,
        .y      = zero_region_offset,
        .width  = u32_size( frame_width ),
        .height = u32_size( frame_height ),
    };
    const grab::platform::x11::CursorImage cursor{
        .width     = u32_size( cursor_width ),
        .height    = u32_size( cursor_height ),
        .hotspot_x = cursor_hotspot_x,
        .hotspot_y = cursor_hotspot_y,
        .xhot      = no_hotspot_offset,
        .yhot      = no_hotspot_offset,
        .pixels    = std::span<const std::uint32_t>{ pixels },
    };

    grab::platform::x11::composite_cursor( std::span<std::uint8_t>{ frame },
                                           frame_stride,
                                           region,
                                           cursor );

    expect_frame_pixel( frame,
                        frame_stride,
                        frame_target_x,
                        frame_target_y,
                        expected_blue,
                        expected_green,
                        expected_red );
}

TEST( X11CursorOverlay,
      CursorClippedAtRightBottomChangesOnlyCoveredPixels )
{
    constexpr std::size_t  frame_width        = 3U;
    constexpr std::size_t  frame_height       = 3U;
    constexpr std::size_t  frame_stride       = frame_width * bgr0_bytes_per_pixel;
    constexpr std::size_t  frame_byte_count   = frame_stride * frame_height;
    constexpr std::size_t  cursor_width       = 2U;
    constexpr std::size_t  cursor_height      = 2U;
    constexpr std::size_t  cursor_pixel_count = cursor_width * cursor_height;
    constexpr std::int32_t cursor_hotspot_x   = 2;
    constexpr std::int32_t cursor_hotspot_y   = 2;
    constexpr std::size_t  visible_frame_x    = 2U;
    constexpr std::size_t  visible_frame_y    = 2U;

    std::array<std::uint8_t, frame_byte_count> frame{};
    fill_frame( frame,
                frame_stride,
                frame_width,
                frame_height,
                initial_blue,
                initial_green,
                initial_red );
    auto expected = frame;
    set_frame_pixel( expected,
                     frame_stride,
                     visible_frame_x,
                     visible_frame_y,
                     opaque_first_blue,
                     opaque_first_green,
                     opaque_first_red );

    constexpr std::array<std::uint32_t, cursor_pixel_count> pixels{
        cursor_pixel( opaque_first_blue,
                      opaque_first_green,
                      opaque_first_red,
                      opaque_alpha ),
        cursor_pixel( opaque_second_blue,
                      opaque_second_green,
                      opaque_second_red,
                      opaque_alpha ),
        cursor_pixel( opaque_third_blue,
                      opaque_third_green,
                      opaque_third_red,
                      opaque_alpha ),
        cursor_pixel( opaque_fourth_blue,
                      opaque_fourth_green,
                      opaque_fourth_red,
                      opaque_alpha ),
    };
    const grab::geometry::Rectangle region{
        .x      = zero_region_offset,
        .y      = zero_region_offset,
        .width  = u32_size( frame_width ),
        .height = u32_size( frame_height ),
    };
    const grab::platform::x11::CursorImage cursor{
        .width     = u32_size( cursor_width ),
        .height    = u32_size( cursor_height ),
        .hotspot_x = cursor_hotspot_x,
        .hotspot_y = cursor_hotspot_y,
        .xhot      = no_hotspot_offset,
        .yhot      = no_hotspot_offset,
        .pixels    = std::span<const std::uint32_t>{ pixels },
    };

    grab::platform::x11::composite_cursor( std::span<std::uint8_t>{ frame },
                                           frame_stride,
                                           region,
                                           cursor );

    EXPECT_EQ( frame, expected );
}

TEST( X11CursorOverlay,
      OutOfBoundsCursorLeavesFrameUnchanged )
{
    constexpr std::size_t  frame_width        = 2U;
    constexpr std::size_t  frame_height       = 2U;
    constexpr std::size_t  frame_stride       = frame_width * bgr0_bytes_per_pixel;
    constexpr std::size_t  frame_byte_count   = frame_stride * frame_height;
    constexpr std::size_t  cursor_width       = 1U;
    constexpr std::size_t  cursor_height      = 1U;
    constexpr std::size_t  cursor_pixel_count = cursor_width * cursor_height;
    constexpr std::int32_t cursor_hotspot_x   = 2;
    constexpr std::int32_t cursor_hotspot_y   = 2;

    std::array<std::uint8_t, frame_byte_count> frame{};
    fill_frame( frame,
                frame_stride,
                frame_width,
                frame_height,
                initial_blue,
                initial_green,
                initial_red );
    const auto                                    expected = frame;

    std::array<std::uint32_t, cursor_pixel_count> pixels{};
    pixels.fill( cursor_pixel( opaque_first_blue,
                               opaque_first_green,
                               opaque_first_red,
                               opaque_alpha ) );
    const grab::geometry::Rectangle region{
        .x      = zero_region_offset,
        .y      = zero_region_offset,
        .width  = u32_size( frame_width ),
        .height = u32_size( frame_height ),
    };
    const grab::platform::x11::CursorImage cursor{
        .width     = u32_size( cursor_width ),
        .height    = u32_size( cursor_height ),
        .hotspot_x = cursor_hotspot_x,
        .hotspot_y = cursor_hotspot_y,
        .xhot      = no_hotspot_offset,
        .yhot      = no_hotspot_offset,
        .pixels    = std::span<const std::uint32_t>{ pixels },
    };

    grab::platform::x11::composite_cursor( std::span<std::uint8_t>{ frame },
                                           frame_stride,
                                           region,
                                           cursor );

    EXPECT_EQ( frame, expected );
}
