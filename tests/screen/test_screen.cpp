#include "codec/png.hpp"
#include "grab/image.hpp"
#include "grab/result.hpp"
#include "grab/screen.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <xcb/xcb.h>
#include <xcb/xproto.h>
// clang-format on

namespace
{

    constexpr const char*      kXvfbDisplay         = ":89";
    constexpr const char*      kBadDisplay          = ":bad-nonexistent-89";
    constexpr int              kXcbOk               = 0;
    constexpr int              kInvalidScreenIndex  = 0;
    constexpr std::int16_t     kWindowX             = 72;
    constexpr std::int16_t     kWindowY             = 88;
    constexpr std::uint16_t    kWindowWidth         = 176U;
    constexpr std::uint16_t    kWindowHeight        = 112U;
    constexpr std::uint16_t    kWindowBorderWidth   = 0U;
    constexpr std::uint16_t    kXvfbWidth           = 1'280U;
    constexpr std::uint16_t    kXvfbHeight          = 1'024U;
    constexpr std::uint32_t    kWindowValueMask     = XCB_CW_BACK_PIXEL;
    constexpr std::uint32_t    kPropertyReplaceMode = XCB_PROP_MODE_REPLACE;
    constexpr std::uint32_t    kKnownColor          = 0X00'2A'7C'E1U;
    constexpr std::uint32_t    kOtherColor          = 0X00'E1'7C'2AU;
    constexpr std::uint8_t     kFormat8Bits         = 8U;
    constexpr std::uint8_t     kFormat32Bits        = 32U;
    constexpr std::uint8_t     kExpectedRed         = 0X2AU;
    constexpr std::uint8_t     kExpectedGreen       = 0X7CU;
    constexpr std::uint8_t     kExpectedBlue        = 0XE1U;
    constexpr std::uint32_t    kSampleX             = kWindowWidth / 2U;
    constexpr std::uint32_t    kSampleY             = kWindowHeight / 2U;
    constexpr std::size_t      kSingleWindowCount   = 1U;
    constexpr auto             kPaintDelay          = std::chrono::milliseconds{ 50 };
    constexpr std::string_view kKnownInstance       = "grab-screen-instance";
    constexpr std::string_view kKnownClass          = "GrabScreenKnownClass";
    constexpr std::string_view kKnownClassCandidate = "screenknown";
    constexpr std::string_view kOtherInstance       = "grab-screen-other-instance";
    constexpr std::string_view kOtherClass          = "GrabScreenOtherClass";
    constexpr std::string_view kMissingClass        = "class-that-does-not-exist";
    constexpr std::string_view kNetClientListAtom   = "_NET_CLIENT_LIST";

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
            int               screen_index_ = kInvalidScreenIndex;
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
            xcb_connection_has_error( connection ) != kXcbOk )
        {
            return testing::AssertionFailure() << "xcb_flush failed";
        }
        return testing::AssertionSuccess();
    }

    [[nodiscard]]
    testing::AssertionResult
    intern_atom( xcb_connection_t* connection,
                 std::string_view  name,
                 xcb_atom_t&       atom )
    {
        xcb_generic_error_t* raw_error = nullptr;
        const auto           reply     = take_xcb_owned( xcb_intern_atom_reply(
            connection,
            xcb_intern_atom( connection,
                             0U,
                             static_cast<std::uint16_t>( name.size() ),
                             name.data() ),
            &raw_error
        ) );
        const auto           error     = take_xcb_owned( raw_error );
        if( error != nullptr || reply == nullptr )
        {
            return testing::AssertionFailure() << "xcb_intern_atom failed";
        }
        atom = reply->atom;
        return testing::AssertionSuccess();
    }

    [[nodiscard]]
    std::string
    wm_class_value( std::string_view instance,
                    std::string_view class_name )
    {
        std::string value{ instance };
        value.push_back( '\0' );
        value.append( class_name );
        value.push_back( '\0' );
        return value;
    }

    void
    set_wm_class( xcb_connection_t* connection,
                  xcb_window_t      window,
                  std::string_view  instance,
                  std::string_view  class_name )
    {
        const std::string value = wm_class_value( instance, class_name );
        EXPECT_TRUE( request_succeeded(
            connection,
            xcb_change_property_checked( connection,
                                         kPropertyReplaceMode,
                                         window,
                                         XCB_ATOM_WM_CLASS,
                                         XCB_ATOM_STRING,
                                         kFormat8Bits,
                                         static_cast<std::uint32_t>( value.size() ),
                                         value.data() )
        ) );
    }

    void
    set_client_list( xcb_connection_t* connection,
                     xcb_window_t      root,
                     xcb_window_t      window )
    {
        xcb_atom_t net_client_list = XCB_ATOM_NONE;
        ASSERT_TRUE( intern_atom( connection, kNetClientListAtom, net_client_list ) );
        const std::array<xcb_window_t, kSingleWindowCount> windows{ window };
        EXPECT_TRUE( request_succeeded(
            connection,
            xcb_change_property_checked( connection,
                                         kPropertyReplaceMode,
                                         root,
                                         net_client_list,
                                         XCB_ATOM_WINDOW,
                                         kFormat32Bits,
                                         static_cast<std::uint32_t>( windows.size() ),
                                         windows.data() )
        ) );
    }

    [[nodiscard]]
    xcb_window_t
    create_solid_window( xcb_connection_t*   connection,
                         const xcb_screen_t& screen,
                         std::uint32_t       color,
                         std::string_view    instance,
                         std::string_view    class_name )
    {
        const xcb_window_t                 window = xcb_generate_id( connection );
        const std::array<std::uint32_t, 1> values{ color };
        EXPECT_TRUE(
            request_succeeded( connection,
                               xcb_create_window_checked( connection,
                                                          screen.root_depth,
                                                          window,
                                                          screen.root,
                                                          kWindowX,
                                                          kWindowY,
                                                          kWindowWidth,
                                                          kWindowHeight,
                                                          kWindowBorderWidth,
                                                          XCB_WINDOW_CLASS_INPUT_OUTPUT,
                                                          screen.root_visual,
                                                          kWindowValueMask,
                                                          values.data() ) )
        );
        set_wm_class( connection, window, instance, class_name );
        EXPECT_TRUE( request_succeeded( connection,
                                        xcb_map_window_checked( connection, window ) ) );
        set_client_list( connection, screen.root, window );
        EXPECT_TRUE( flush_succeeded( connection ) );
        std::this_thread::sleep_for( kPaintDelay );
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

    [[nodiscard]]
    bool
    is_supported_capture_format( grab::PixelFormat format ) noexcept
    {
        return format ==
               grab::PixelFormat::bgra ||
               format ==
               grab::PixelFormat::rgba ||
               format ==
               grab::PixelFormat::bgr ||
               format == grab::PixelFormat::rgb;
    }

    struct ExpectedChannels
    {
            std::uint8_t first  = 0U;
            std::uint8_t second = 0U;
            std::uint8_t third  = 0U;
    };

    [[nodiscard]]
    ExpectedChannels
    expected_channels_for( grab::PixelFormat format ) noexcept
    {
        switch( format )
        {
            case grab::PixelFormat::bgra :
            case grab::PixelFormat::bgr :
            case grab::PixelFormat::bgr0 :
                return ExpectedChannels{
                    .first  = kExpectedBlue,
                    .second = kExpectedGreen,
                    .third  = kExpectedRed,
                };
            case grab::PixelFormat::rgba :
            case grab::PixelFormat::rgb :
            case grab::PixelFormat::rgb24 :
                return ExpectedChannels{
                    .first  = kExpectedRed,
                    .second = kExpectedGreen,
                    .third  = kExpectedBlue,
                };
            case grab::PixelFormat::gray :
                return {};
        }
        return {};
    }

    void
    expect_sample_matches_known_color( const grab::Image& image )
    {
        ASSERT_EQ( image.width, kWindowWidth );
        ASSERT_EQ( image.height, kWindowHeight );
        ASSERT_TRUE( is_supported_capture_format( image.format ) );

        const ExpectedChannels expected = expected_channels_for( image.format );
        EXPECT_EQ( byte_at( image, kSampleX, kSampleY, 0U ), expected.first );
        EXPECT_EQ( byte_at( image, kSampleX, kSampleY, 1U ), expected.second );
        EXPECT_EQ( byte_at( image, kSampleX, kSampleY, 2U ), expected.third );
    }

}    // namespace

TEST( Screen,
      CapturesWindowByClass )
{
    const TestConnection connection{ kXvfbDisplay };
    ASSERT_NE( connection.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( connection.get() ), kXcbOk );
    const xcb_screen_t* screen_info =
        default_screen( connection.get(), connection.screen_index() );
    ASSERT_NE( screen_info, nullptr );
    static_cast<void>( create_solid_window( connection.get(),
                                            *screen_info,
                                            kKnownColor,
                                            kKnownInstance,
                                            kKnownClass ) );

    auto screen = grab::Screen::open( kXvfbDisplay );
    ASSERT_TRUE( screen.has_value() ) << screen.error().message;
    const std::vector<std::string> candidates{ std::string{ kKnownClassCandidate } };
    auto                           image = screen->window_by_class( candidates );

    ASSERT_TRUE( image.has_value() ) << image.error().message;
    expect_sample_matches_known_color( *image );

    auto encoded = grab::codec::encode_png( *image );
    ASSERT_TRUE( encoded.has_value() ) << encoded.error().message;
    ASSERT_FALSE( encoded->empty() );

    auto decoded = grab::codec::decode_png( *encoded );
    ASSERT_TRUE( decoded.has_value() ) << decoded.error().message;
    EXPECT_EQ( decoded->width, kWindowWidth );
    EXPECT_EQ( decoded->height, kWindowHeight );
}

TEST( Screen,
      CaptureDisplayEncodesToPng )
{
    auto screen = grab::Screen::open( kXvfbDisplay );
    ASSERT_TRUE( screen.has_value() ) << screen.error().message;

    auto image = screen->display();

    ASSERT_TRUE( image.has_value() ) << image.error().message;
    EXPECT_EQ( image->width, kXvfbWidth );
    EXPECT_EQ( image->height, kXvfbHeight );

    auto encoded = grab::codec::encode_png( *image );
    ASSERT_TRUE( encoded.has_value() ) << encoded.error().message;
    ASSERT_FALSE( encoded->empty() );

    auto decoded = grab::codec::decode_png( *encoded );
    ASSERT_TRUE( decoded.has_value() ) << decoded.error().message;
    EXPECT_EQ( decoded->width, kXvfbWidth );
    EXPECT_EQ( decoded->height, kXvfbHeight );
}

TEST( Screen,
      WindowByClassNotFound )
{
    const TestConnection connection{ kXvfbDisplay };
    ASSERT_NE( connection.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( connection.get() ), kXcbOk );
    const xcb_screen_t* screen_info =
        default_screen( connection.get(), connection.screen_index() );
    ASSERT_NE( screen_info, nullptr );
    static_cast<void>( create_solid_window( connection.get(),
                                            *screen_info,
                                            kOtherColor,
                                            kOtherInstance,
                                            kOtherClass ) );
    auto screen = grab::Screen::open( kXvfbDisplay );
    ASSERT_TRUE( screen.has_value() ) << screen.error().message;
    const std::vector<std::string> candidates{ std::string{ kMissingClass } };

    auto                           image = screen->window_by_class( candidates );

    ASSERT_FALSE( image.has_value() );
    EXPECT_EQ( image.error().code, grab::ErrorCode::window_not_found );
}

TEST( Screen,
      OpenFailsOnBadDisplay )
{
    auto screen = grab::Screen::open( kBadDisplay );

    ASSERT_FALSE( screen.has_value() );
    EXPECT_EQ( screen.error().code, grab::ErrorCode::device_inaccessible );
}
