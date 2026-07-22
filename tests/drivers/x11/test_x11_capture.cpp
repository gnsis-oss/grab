#include "drivers/desktop/x11/x11_capture.hpp"
#include "grab/image.hpp"
#include "grab/result.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <thread>
#include <xcb/xcb.h>
#include <xcb/xproto.h>
// clang-format on

namespace
{

    constexpr const char*   xvfbDisplay     = ":93";
    constexpr const char*   badDisplay      = ":bad-nonexistent-93";
    constexpr int           xcbOk           = 0;
    constexpr std::int16_t  windowX         = 48;
    constexpr std::int16_t  windowY         = 64;
    constexpr std::int16_t  occluderX       = 48;
    constexpr std::int16_t  occluderY       = 64;
    constexpr std::uint16_t windowWidth     = 160U;
    constexpr std::uint16_t windowHeight    = 96U;
    constexpr std::uint16_t occluderWidth   = 200U;
    constexpr std::uint16_t occluderHeight  = 128U;
    constexpr std::uint16_t windowBorder    = 0U;
    constexpr std::uint16_t xvfbWidth       = 1'280U;
    constexpr std::uint16_t xvfbHeight      = 1'024U;
    constexpr std::uint32_t knownColor      = 0X00'33'66'CCU;
    constexpr std::uint32_t occluderColor   = 0X00'00'CC'33U;
    constexpr std::uint32_t windowValueMask = XCB_CW_BACK_PIXEL;
    constexpr std::uint8_t  expectedRed     = 0X33U;
    constexpr std::uint8_t  expectedGreen   = 0X66U;
    constexpr std::uint8_t  expectedBlue    = 0XCCU;
    constexpr auto          paintDelay      = std::chrono::milliseconds{ 50 };

    template<typename T>
    using XcbOwned = std::unique_ptr<T, decltype( &std::free )>;

    template<typename T>
    [[nodiscard]]
    XcbOwned<T>
    take_xcb_owned( T* pointer ) noexcept
    {
        return XcbOwned<T>{ pointer, &std::free };
    }

    class TestConnection
    {
        public:

            explicit TestConnection( const char* display ) :
                connection_( xcb_connect( display,
                                          &screen_index_ ) )
            {
            }

            ~TestConnection()
            {
                if( connection_ != nullptr )
                {
                    xcb_disconnect( connection_ );
                }
            }

            TestConnection( const TestConnection& ) = delete;
            TestConnection&
            operator=( const TestConnection& ) = delete;
            TestConnection( TestConnection&& ) = delete;
            TestConnection&
            operator=( TestConnection&& ) = delete;

            [[nodiscard]]
            xcb_connection_t*
            get() const noexcept
            {
                return connection_;
            }

            [[nodiscard]]
            int
            screen_index() const noexcept
            {
                return screen_index_;
            }

        private:

            xcb_connection_t* connection_   = nullptr;
            int               screen_index_ = 0;
    };

    [[nodiscard]]
    const xcb_screen_t*
    default_screen( xcb_connection_t* connection,
                    int               screen_index )
    {
        xcb_screen_iterator_t iterator =
            xcb_setup_roots_iterator( xcb_get_setup( connection ) );
        for( int current_screen = 0; current_screen < screen_index && iterator.rem > 0;
             ++current_screen )
        {
            xcb_screen_next( &iterator );
        }
        return iterator.data;
    }

    [[nodiscard]]
    testing::AssertionResult
    request_succeeded( xcb_connection_t* connection,
                       xcb_void_cookie_t cookie )
    {
        const auto error = take_xcb_owned( xcb_request_check( connection, cookie ) );
        if( error != nullptr )
        {
            return testing::AssertionFailure()
                << "X error " << static_cast<unsigned int>( error->error_code );
        }
        return testing::AssertionSuccess();
    }

    [[nodiscard]]
    testing::AssertionResult
    flush_succeeded( xcb_connection_t* connection )
    {
        if( xcb_flush( connection ) <=
            0 ||
            xcb_connection_has_error( connection ) != xcbOk )
        {
            return testing::AssertionFailure() << "xcb_flush failed";
        }
        return testing::AssertionSuccess();
    }

    [[nodiscard]]
    xcb_window_t
    create_solid_window( xcb_connection_t*   connection,
                         const xcb_screen_t& screen,
                         std::int16_t        x,
                         std::int16_t        y,
                         std::uint16_t       width,
                         std::uint16_t       height,
                         std::uint32_t       color )
    {
        const xcb_window_t                 window = xcb_generate_id( connection );
        const std::array<std::uint32_t, 1> values{ color };
        EXPECT_TRUE(
            request_succeeded( connection,
                               xcb_create_window_checked( connection,
                                                          screen.root_depth,
                                                          window,
                                                          screen.root,
                                                          x,
                                                          y,
                                                          width,
                                                          height,
                                                          windowBorder,
                                                          XCB_WINDOW_CLASS_INPUT_OUTPUT,
                                                          screen.root_visual,
                                                          windowValueMask,
                                                          values.data() ) )
        );
        EXPECT_TRUE( request_succeeded( connection,
                                        xcb_map_window_checked( connection, window ) ) );
        EXPECT_TRUE( flush_succeeded( connection ) );
        std::this_thread::sleep_for( paintDelay );
        return window;
    }

    [[nodiscard]]
    std::uint8_t
    byte_at( const grab::Image& image,
             std::uint32_t      x,
             std::uint32_t      y,
             std::uint32_t      channel )
    {
        const auto offset =
            ( static_cast<std::size_t>( y ) * image.stride ) +
            ( static_cast<std::size_t>( x ) * grab::bytes_per_pixel( image.format ) ) +
            channel;
        return std::to_integer<std::uint8_t>( image.pixels.at( offset ) );
    }

    void
    expect_sample_matches_known_color( const grab::Image& image )
    {
        constexpr std::uint32_t sampleX = windowWidth / 2U;
        constexpr std::uint32_t sampleY = windowHeight / 2U;
        ASSERT_GE( image.width, windowWidth );
        ASSERT_GE( image.height, windowHeight );
        ASSERT_TRUE( image.format ==
                     grab::PixelFormat::Bgra ||
                     image.format == grab::PixelFormat::Bgr );
        EXPECT_EQ( byte_at( image, sampleX, sampleY, 0U ), expectedBlue );
        EXPECT_EQ( byte_at( image, sampleX, sampleY, 1U ), expectedGreen );
        EXPECT_EQ( byte_at( image, sampleX, sampleY, 2U ), expectedRed );
    }

}    // namespace

TEST( X11Capturer,
      CapturesSolidColorWindow )
{
    const TestConnection connection{ xvfbDisplay };
    ASSERT_NE( connection.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( connection.get() ), xcbOk );
    const xcb_screen_t* screen =
        default_screen( connection.get(), connection.screen_index() );
    ASSERT_NE( screen, nullptr );
    const xcb_window_t window   = create_solid_window( connection.get(),
                                                       *screen,
                                                       windowX,
                                                       windowY,
                                                       windowWidth,
                                                       windowHeight,
                                                       knownColor );

    auto               capturer = grab::screen::X11Capturer::open( xvfbDisplay );
    ASSERT_TRUE( capturer.has_value() ) << capturer.error().message;
    auto image = capturer->capture_window( window );

    ASSERT_TRUE( image.has_value() ) << image.error().message;
    EXPECT_EQ( image->width, windowWidth );
    EXPECT_EQ( image->height, windowHeight );
    expect_sample_matches_known_color( *image );
}

TEST( X11Capturer,
      CapturesOccludedWindowWithoutRaising )
{
    const TestConnection connection{ xvfbDisplay };
    ASSERT_NE( connection.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( connection.get() ), xcbOk );
    const xcb_screen_t* screen =
        default_screen( connection.get(), connection.screen_index() );
    ASSERT_NE( screen, nullptr );
    const xcb_window_t target = create_solid_window( connection.get(),
                                                     *screen,
                                                     windowX,
                                                     windowY,
                                                     windowWidth,
                                                     windowHeight,
                                                     knownColor );
    static_cast<void>( create_solid_window( connection.get(),
                                            *screen,
                                            occluderX,
                                            occluderY,
                                            occluderWidth,
                                            occluderHeight,
                                            occluderColor ) );

    auto capturer = grab::screen::X11Capturer::open( xvfbDisplay );
    ASSERT_TRUE( capturer.has_value() ) << capturer.error().message;
    auto image = capturer->capture_window( target );

    ASSERT_TRUE( image.has_value() ) << image.error().message;
    EXPECT_EQ( image->width, windowWidth );
    EXPECT_EQ( image->height, windowHeight );
    expect_sample_matches_known_color( *image );
}

TEST( X11Capturer,
      CaptureDisplayHasScreenDimensions )
{
    auto capturer = grab::screen::X11Capturer::open( xvfbDisplay );
    ASSERT_TRUE( capturer.has_value() ) << capturer.error().message;

    auto image = capturer->capture_display();

    ASSERT_TRUE( image.has_value() ) << image.error().message;
    EXPECT_EQ( image->width, xvfbWidth );
    EXPECT_EQ( image->height, xvfbHeight );
}

TEST( X11Capturer,
      OpenFailsOnBadDisplay )
{
    auto capturer = grab::screen::X11Capturer::open( badDisplay );

    ASSERT_FALSE( capturer.has_value() );
    EXPECT_EQ( capturer.error().code, grab::ErrorCode::DeviceInaccessible );
}
