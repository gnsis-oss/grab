#include "codec/png.hpp"
#include "grab/geometry/rectangle.hpp"
#include "grab/image.hpp"
#include "grab/result.hpp"
#include "grab/screen.hpp"
#include "grab/window_info.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
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

    constexpr const char*      xvfbDisplay         = ":89";
    constexpr const char*      badDisplay          = ":bad-nonexistent-89";
    constexpr int              xcbOk               = 0;
    constexpr int              invalidScreenIndex  = 0;
    constexpr std::int16_t     windowX             = 72;
    constexpr std::int16_t     windowY             = 88;
    constexpr std::uint16_t    windowWidth         = 176U;
    constexpr std::uint16_t    windowHeight        = 112U;
    constexpr std::uint16_t    windowBorderWidth   = 0U;
    constexpr std::uint16_t    xvfbWidth           = 1'280U;
    constexpr std::uint16_t    xvfbHeight          = 1'024U;
    constexpr std::uint32_t    windowValueMask     = XCB_CW_BACK_PIXEL;
    constexpr std::uint32_t    propertyReplaceMode = XCB_PROP_MODE_REPLACE;
    constexpr std::uint32_t    knownColor          = 0X00'2A'7C'E1U;
    constexpr std::uint32_t    otherColor          = 0X00'E1'7C'2AU;
    constexpr std::uint8_t     format8Bits         = 8U;
    constexpr std::uint8_t     format32Bits        = 32U;
    constexpr std::uint8_t     expectedRed         = 0X2AU;
    constexpr std::uint8_t     expectedGreen       = 0X7CU;
    constexpr std::uint8_t     expectedBlue        = 0XE1U;
    constexpr std::uint32_t    sampleX             = windowWidth / 2U;
    constexpr std::uint32_t    sampleY             = windowHeight / 2U;
    constexpr std::size_t      singleWindowCount   = 1U;
    constexpr std::int32_t     placedX             = 240;
    constexpr std::int32_t     placedY             = 160;
    constexpr std::uint32_t    placedWidth         = 320U;
    constexpr std::uint32_t    placedHeight        = 200U;
    // An id no client on the fixture display can hold.
    constexpr std::uint32_t    anyWindowId          = 0X0F'FF'FF'FFU;
    constexpr auto             placementTestTimeout = std::chrono::milliseconds{ 200 };
    constexpr auto             paintDelay           = std::chrono::milliseconds{ 50 };
    constexpr std::string_view knownInstance        = "grab-screen-instance";
    constexpr std::string_view knownClass           = "GrabScreenKnownClass";
    constexpr std::string_view knownClassCandidate  = "screenknown";
    constexpr std::string_view otherInstance        = "grab-screen-other-instance";
    constexpr std::string_view otherClass           = "GrabScreenOtherClass";
    constexpr std::string_view missingClass         = "class-that-does-not-exist";
    constexpr std::string_view netClientListAtom    = "_NET_CLIENT_LIST";
    constexpr std::string_view netActiveWindowAtom   = "_NET_ACTIVE_WINDOW";
    constexpr std::string_view netWmWindowTypeAtom  = "_NET_WM_WINDOW_TYPE";
    constexpr std::string_view splashTypeAtom       = "_NET_WM_WINDOW_TYPE_SPLASH";
    constexpr std::string_view normalTypeAtom       = "_NET_WM_WINDOW_TYPE_NORMAL";
    constexpr std::string_view splashTypeName       = "splash";
    constexpr std::string_view normalTypeName       = "normal";
    constexpr std::size_t      declaredTypeCount    = 2U;

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
            int               screen_index_ = invalidScreenIndex;
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
                                         propertyReplaceMode,
                                         window,
                                         XCB_ATOM_WM_CLASS,
                                         XCB_ATOM_STRING,
                                         format8Bits,
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
        ASSERT_TRUE( intern_atom( connection, netClientListAtom, net_client_list ) );
        const std::array<xcb_window_t, singleWindowCount> windows{ window };
        EXPECT_TRUE( request_succeeded(
            connection,
            xcb_change_property_checked( connection,
                                         propertyReplaceMode,
                                         root,
                                         net_client_list,
                                         XCB_ATOM_WINDOW,
                                         format32Bits,
                                         static_cast<std::uint32_t>( windows.size() ),
                                         windows.data() )
        ) );
    }

    void
    set_active_window( xcb_connection_t* connection,
                       xcb_window_t      root,
                       xcb_window_t      window )
    {
        xcb_atom_t net_active_window = XCB_ATOM_NONE;
        ASSERT_TRUE( intern_atom( connection, netActiveWindowAtom, net_active_window ) );
        const std::array<xcb_window_t, singleWindowCount> windows{ window };
        EXPECT_TRUE( request_succeeded(
            connection,
            xcb_change_property_checked( connection,
                                         propertyReplaceMode,
                                         root,
                                         net_active_window,
                                         XCB_ATOM_WINDOW,
                                         format32Bits,
                                         static_cast<std::uint32_t>( windows.size() ),
                                         windows.data() )
        ) );
    }

    // Removes any _NET_ACTIVE_WINDOW a previous test left on the shared display's
    // root, so "no window is active" is a state this suite can actually reach.
    void
    clear_active_window( xcb_connection_t* connection,
                         xcb_window_t      root )
    {
        xcb_atom_t net_active_window = XCB_ATOM_NONE;
        ASSERT_TRUE( intern_atom( connection, netActiveWindowAtom, net_active_window ) );
        EXPECT_TRUE( request_succeeded(
            connection,
            xcb_delete_property_checked( connection, root, net_active_window )
        ) );
    }

    // Declares the type list a splash screen really publishes: its own type
    // first, then NORMAL as a fallback for window managers that do not implement
    // SPLASH. Only the ordering distinguishes it from a plain window.
    void
    set_splash_window_type( xcb_connection_t* connection,
                            xcb_window_t      window )
    {
        xcb_atom_t window_type = XCB_ATOM_NONE;
        xcb_atom_t splash      = XCB_ATOM_NONE;
        xcb_atom_t normal      = XCB_ATOM_NONE;
        ASSERT_TRUE( intern_atom( connection, netWmWindowTypeAtom, window_type ) );
        ASSERT_TRUE( intern_atom( connection, splashTypeAtom, splash ) );
        ASSERT_TRUE( intern_atom( connection, normalTypeAtom, normal ) );

        const std::array<xcb_atom_t, declaredTypeCount> declared{ splash, normal };
        EXPECT_TRUE( request_succeeded(
            connection,
            xcb_change_property_checked( connection,
                                         propertyReplaceMode,
                                         window,
                                         window_type,
                                         XCB_ATOM_ATOM,
                                         format32Bits,
                                         static_cast<std::uint32_t>( declared.size() ),
                                         declared.data() )
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
                                                          windowX,
                                                          windowY,
                                                          windowWidth,
                                                          windowHeight,
                                                          windowBorderWidth,
                                                          XCB_WINDOW_CLASS_INPUT_OUTPUT,
                                                          screen.root_visual,
                                                          windowValueMask,
                                                          values.data() ) )
        );
        set_wm_class( connection, window, instance, class_name );
        EXPECT_TRUE( request_succeeded( connection,
                                        xcb_map_window_checked( connection, window ) ) );
        set_client_list( connection, screen.root, window );
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

    [[nodiscard]]
    bool
    is_supported_capture_format( grab::PixelFormat format ) noexcept
    {
        return format ==
               grab::PixelFormat::Bgra ||
               format ==
               grab::PixelFormat::Rgba ||
               format ==
               grab::PixelFormat::Bgr ||
               format == grab::PixelFormat::Rgb;
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
            case grab::PixelFormat::Bgra :
            case grab::PixelFormat::Bgr :
                return ExpectedChannels{
                    .first  = expectedBlue,
                    .second = expectedGreen,
                    .third  = expectedRed,
                };
            case grab::PixelFormat::Rgba :
            case grab::PixelFormat::Rgb :
                return ExpectedChannels{
                    .first  = expectedRed,
                    .second = expectedGreen,
                    .third  = expectedBlue,
                };
            case grab::PixelFormat::Gray :
                return {};
        }
        return {};
    }

    void
    expect_sample_matches_known_color( const grab::Image& image )
    {
        ASSERT_EQ( image.width, windowWidth );
        ASSERT_EQ( image.height, windowHeight );
        ASSERT_TRUE( is_supported_capture_format( image.format ) );

        const ExpectedChannels expected = expected_channels_for( image.format );
        EXPECT_EQ( byte_at( image, sampleX, sampleY, 0U ), expected.first );
        EXPECT_EQ( byte_at( image, sampleX, sampleY, 1U ), expected.second );
        EXPECT_EQ( byte_at( image, sampleX, sampleY, 2U ), expected.third );
    }

}    // namespace

TEST( Screen,
      CapturesWindowByClass )
{
    const TestConnection connection{ xvfbDisplay };
    ASSERT_NE( connection.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( connection.get() ), xcbOk );
    const xcb_screen_t* screen_info =
        default_screen( connection.get(), connection.screen_index() );
    ASSERT_NE( screen_info, nullptr );
    static_cast<void>( create_solid_window( connection.get(),
                                            *screen_info,
                                            knownColor,
                                            knownInstance,
                                            knownClass ) );

    auto screen = grab::Screen::open( xvfbDisplay );
    ASSERT_TRUE( screen.has_value() ) << screen.error().message;
    const std::vector<std::string> candidates{ std::string{ knownClassCandidate } };
    auto                           image = screen->window_by_class( candidates );

    ASSERT_TRUE( image.has_value() ) << image.error().message;
    expect_sample_matches_known_color( *image );

    auto encoded = grab::codec::encode_png( *image );
    ASSERT_TRUE( encoded.has_value() ) << encoded.error().message;
    ASSERT_FALSE( encoded->empty() );

    auto decoded = grab::codec::decode_png( *encoded );
    ASSERT_TRUE( decoded.has_value() ) << decoded.error().message;
    EXPECT_EQ( decoded->width, windowWidth );
    EXPECT_EQ( decoded->height, windowHeight );
}

TEST( Screen,
      CaptureDisplayEncodesToPng )
{
    auto screen = grab::Screen::open( xvfbDisplay );
    ASSERT_TRUE( screen.has_value() ) << screen.error().message;

    auto image = screen->display();

    ASSERT_TRUE( image.has_value() ) << image.error().message;
    EXPECT_EQ( image->width, xvfbWidth );
    EXPECT_EQ( image->height, xvfbHeight );

    auto encoded = grab::codec::encode_png( *image );
    ASSERT_TRUE( encoded.has_value() ) << encoded.error().message;
    ASSERT_FALSE( encoded->empty() );

    auto decoded = grab::codec::decode_png( *encoded );
    ASSERT_TRUE( decoded.has_value() ) << decoded.error().message;
    EXPECT_EQ( decoded->width, xvfbWidth );
    EXPECT_EQ( decoded->height, xvfbHeight );
}

TEST( Screen,
      WindowByClassNotFound )
{
    const TestConnection connection{ xvfbDisplay };
    ASSERT_NE( connection.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( connection.get() ), xcbOk );
    const xcb_screen_t* screen_info =
        default_screen( connection.get(), connection.screen_index() );
    ASSERT_NE( screen_info, nullptr );
    static_cast<void>( create_solid_window( connection.get(),
                                            *screen_info,
                                            otherColor,
                                            otherInstance,
                                            otherClass ) );
    auto screen = grab::Screen::open( xvfbDisplay );
    ASSERT_TRUE( screen.has_value() ) << screen.error().message;
    const std::vector<std::string> candidates{ std::string{ missingClass } };

    auto                           image = screen->window_by_class( candidates );

    ASSERT_FALSE( image.has_value() );
    EXPECT_EQ( image.error().code, grab::ErrorCode::WindowNotFound );
}

TEST( Screen,
      ListsWindowsWithBoundsAndClass )
{
    const TestConnection connection{ xvfbDisplay };
    ASSERT_NE( connection.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( connection.get() ), xcbOk );
    const xcb_screen_t* screen_info =
        default_screen( connection.get(), connection.screen_index() );
    ASSERT_NE( screen_info, nullptr );
    const xcb_window_t window = create_solid_window( connection.get(),
                                                     *screen_info,
                                                     knownColor,
                                                     knownInstance,
                                                     knownClass );

    auto               screen = grab::Screen::open( xvfbDisplay );
    ASSERT_TRUE( screen.has_value() ) << screen.error().message;
    auto windows = screen->windows();

    ASSERT_TRUE( windows.has_value() ) << windows.error().message;
    const auto listed = std::ranges::find( *windows,
                                           static_cast<std::uint32_t>( window ),
                                           &grab::WindowSummary::id );
    ASSERT_NE( listed, windows->end() );
    EXPECT_EQ( listed->wm_class, knownClass );
    EXPECT_EQ( listed->bounds.x, windowX );
    EXPECT_EQ( listed->bounds.y, windowY );
    EXPECT_EQ( listed->bounds.width, windowWidth );
    EXPECT_EQ( listed->bounds.height, windowHeight );
}

TEST( Screen,
      ActivateWindowByClassRaisesTheMatch )
{
    const TestConnection connection{ xvfbDisplay };
    ASSERT_NE( connection.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( connection.get() ), xcbOk );
    const xcb_screen_t* screen_info =
        default_screen( connection.get(), connection.screen_index() );
    ASSERT_NE( screen_info, nullptr );
    const xcb_window_t window = create_solid_window( connection.get(),
                                                     *screen_info,
                                                     knownColor,
                                                     knownInstance,
                                                     knownClass );

    auto               screen = grab::Screen::open( xvfbDisplay );
    ASSERT_TRUE( screen.has_value() ) << screen.error().message;
    const std::vector<std::string> candidates{ std::string{ knownClassCandidate } };

    auto focused = screen->activate_window_by_class( candidates );

    ASSERT_TRUE( focused.has_value() ) << focused.error().message;
    EXPECT_EQ( focused->id, static_cast<std::uint32_t>( window ) );
    EXPECT_EQ( focused->wm_class, knownClass );
}

TEST( Screen,
      ActivateWindowByClassReportsAMiss )
{
    auto screen = grab::Screen::open( xvfbDisplay );
    ASSERT_TRUE( screen.has_value() ) << screen.error().message;
    const std::vector<std::string> candidates{ std::string{ missingClass } };

    auto focused = screen->activate_window_by_class( candidates );

    ASSERT_FALSE( focused.has_value() );
    EXPECT_EQ( focused.error().code, grab::ErrorCode::WindowNotFound );
}

TEST( Screen,
      CapturesWindowById )
{
    const TestConnection connection{ xvfbDisplay };
    ASSERT_NE( connection.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( connection.get() ), xcbOk );
    const xcb_screen_t* screen_info =
        default_screen( connection.get(), connection.screen_index() );
    ASSERT_NE( screen_info, nullptr );
    const xcb_window_t window = create_solid_window( connection.get(),
                                                     *screen_info,
                                                     knownColor,
                                                     knownInstance,
                                                     knownClass );

    auto               screen = grab::Screen::open( xvfbDisplay );
    ASSERT_TRUE( screen.has_value() ) << screen.error().message;
    auto image = screen->window_by_id( static_cast<std::uint32_t>( window ) );

    ASSERT_TRUE( image.has_value() ) << image.error().message;
    expect_sample_matches_known_color( *image );
}

TEST( Screen,
      PlaceWindowSettlesOnTheRequestedGeometry )
{
    const TestConnection connection{ xvfbDisplay };
    ASSERT_NE( connection.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( connection.get() ), xcbOk );
    const xcb_screen_t* screen_info =
        default_screen( connection.get(), connection.screen_index() );
    ASSERT_NE( screen_info, nullptr );
    const xcb_window_t window = create_solid_window( connection.get(),
                                                     *screen_info,
                                                     knownColor,
                                                     knownInstance,
                                                     knownClass );

    auto               screen = grab::Screen::open( xvfbDisplay );
    ASSERT_TRUE( screen.has_value() ) << screen.error().message;
    const grab::geometry::Rectangle request{
        .x      = placedX,
        .y      = placedY,
        .width  = placedWidth,
        .height = placedHeight
    };

    auto placed = screen->place_window( static_cast<std::uint32_t>( window ), request );

    ASSERT_TRUE( placed.has_value() ) << placed.error().message;
    EXPECT_EQ( placed->x, request.x );
    EXPECT_EQ( placed->y, request.y );
    EXPECT_EQ( placed->width, request.width );
    EXPECT_EQ( placed->height, request.height );

    // The window really moved, not merely the value this call echoed back.
    auto windows = screen->windows();
    ASSERT_TRUE( windows.has_value() ) << windows.error().message;
    const auto listed = std::ranges::find( *windows,
                                           static_cast<std::uint32_t>( window ),
                                           &grab::WindowSummary::id );
    ASSERT_NE( listed, windows->end() );
    EXPECT_EQ( listed->bounds.x, request.x );
    EXPECT_EQ( listed->bounds.width, request.width );
}

TEST( Screen,
      PlaceWindowRejectsAZeroSizedRequest )
{
    auto screen = grab::Screen::open( xvfbDisplay );
    ASSERT_TRUE( screen.has_value() ) << screen.error().message;
    const grab::geometry::Rectangle
         request{ .x = placedX, .y = placedY, .width = 0U, .height = placedHeight };

    auto placed = screen->place_window( anyWindowId, request );

    ASSERT_FALSE( placed.has_value() );
    EXPECT_EQ( placed.error().code, grab::ErrorCode::InvalidArgument );
}

// A window that cannot exist can never settle, so the wait has to end in a
// refusal rather than in a report of whatever was last seen.
TEST( Screen,
      PlaceWindowFailsForAnAbsentWindow )
{
    auto screen = grab::Screen::open( xvfbDisplay );
    ASSERT_TRUE( screen.has_value() ) << screen.error().message;
    const grab::geometry::Rectangle request{
        .x      = placedX,
        .y      = placedY,
        .width  = placedWidth,
        .height = placedHeight
    };

    auto placed = screen->place_window( anyWindowId, request, placementTestTimeout );

    ASSERT_FALSE( placed.has_value() );
}

// The splash-screen case: same class and title as the main window, and it also
// advertises NORMAL, so a containment test would report it as normal.
TEST( Screen,
      ReportsTheFirstDeclaredWindowType )
{
    const TestConnection connection{ xvfbDisplay };
    ASSERT_NE( connection.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( connection.get() ), xcbOk );
    const xcb_screen_t* screen_info =
        default_screen( connection.get(), connection.screen_index() );
    ASSERT_NE( screen_info, nullptr );
    const xcb_window_t window = create_solid_window( connection.get(),
                                                     *screen_info,
                                                     knownColor,
                                                     knownInstance,
                                                     knownClass );
    set_splash_window_type( connection.get(), window );
    ASSERT_TRUE( flush_succeeded( connection.get() ) );

    auto screen = grab::Screen::open( xvfbDisplay );
    ASSERT_TRUE( screen.has_value() ) << screen.error().message;
    auto windows = screen->windows();

    ASSERT_TRUE( windows.has_value() ) << windows.error().message;
    const auto listed = std::ranges::find( *windows,
                                           static_cast<std::uint32_t>( window ),
                                           &grab::WindowSummary::id );
    ASSERT_NE( listed, windows->end() );
    EXPECT_EQ( listed->type, splashTypeName );
}

// EWMH prescribes NORMAL for a window that declares no type at all.
TEST( Screen,
      ReportsNormalForAnUntypedWindow )
{
    const TestConnection connection{ xvfbDisplay };
    ASSERT_NE( connection.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( connection.get() ), xcbOk );
    const xcb_screen_t* screen_info =
        default_screen( connection.get(), connection.screen_index() );
    ASSERT_NE( screen_info, nullptr );
    const xcb_window_t window = create_solid_window( connection.get(),
                                                     *screen_info,
                                                     knownColor,
                                                     knownInstance,
                                                     knownClass );

    auto               screen = grab::Screen::open( xvfbDisplay );
    ASSERT_TRUE( screen.has_value() ) << screen.error().message;
    auto windows = screen->windows();

    ASSERT_TRUE( windows.has_value() ) << windows.error().message;
    const auto listed = std::ranges::find( *windows,
                                           static_cast<std::uint32_t>( window ),
                                           &grab::WindowSummary::id );
    ASSERT_NE( listed, windows->end() );
    EXPECT_EQ( listed->type, normalTypeName );
}

TEST( Screen,
      ActiveWindowIdReportsTheActiveWindow )
{
    const TestConnection connection{ xvfbDisplay };
    ASSERT_NE( connection.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( connection.get() ), xcbOk );
    const xcb_screen_t* screen_info =
        default_screen( connection.get(), connection.screen_index() );
    ASSERT_NE( screen_info, nullptr );
    const xcb_window_t window = create_solid_window( connection.get(),
                                                     *screen_info,
                                                     knownColor,
                                                     knownInstance,
                                                     knownClass );
    set_active_window( connection.get(), screen_info->root, window );
    ASSERT_TRUE( flush_succeeded( connection.get() ) );

    auto screen = grab::Screen::open( xvfbDisplay );
    ASSERT_TRUE( screen.has_value() ) << screen.error().message;
    auto active = screen->active_window_id();

    ASSERT_TRUE( active.has_value() ) << active.error().message;
    EXPECT_EQ( *active, static_cast<std::uint32_t>( window ) );
}

TEST( Screen,
      ActiveWindowIdReportsAMissWhenUnset )
{
    const TestConnection connection{ xvfbDisplay };
    ASSERT_NE( connection.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( connection.get() ), xcbOk );
    const xcb_screen_t* screen_info =
        default_screen( connection.get(), connection.screen_index() );
    ASSERT_NE( screen_info, nullptr );
    clear_active_window( connection.get(), screen_info->root );
    ASSERT_TRUE( flush_succeeded( connection.get() ) );

    auto screen = grab::Screen::open( xvfbDisplay );
    ASSERT_TRUE( screen.has_value() ) << screen.error().message;
    auto active = screen->active_window_id();

    ASSERT_FALSE( active.has_value() );
    EXPECT_EQ( active.error().code, grab::ErrorCode::WindowNotFound );
}

TEST( Screen,
      ActiveWindowSummaryDescribesTheActiveWindow )
{
    const TestConnection connection{ xvfbDisplay };
    ASSERT_NE( connection.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( connection.get() ), xcbOk );
    const xcb_screen_t* screen_info =
        default_screen( connection.get(), connection.screen_index() );
    ASSERT_NE( screen_info, nullptr );
    const xcb_window_t window = create_solid_window( connection.get(),
                                                     *screen_info,
                                                     knownColor,
                                                     knownInstance,
                                                     knownClass );
    set_active_window( connection.get(), screen_info->root, window );
    ASSERT_TRUE( flush_succeeded( connection.get() ) );

    auto screen = grab::Screen::open( xvfbDisplay );
    ASSERT_TRUE( screen.has_value() ) << screen.error().message;
    auto summary = screen->active_window_summary();

    ASSERT_TRUE( summary.has_value() ) << summary.error().message;
    EXPECT_EQ( summary->id, static_cast<std::uint32_t>( window ) );
    EXPECT_EQ( summary->wm_class, knownClass );
    EXPECT_EQ( summary->bounds.x, windowX );
    EXPECT_EQ( summary->bounds.y, windowY );
    EXPECT_EQ( summary->bounds.width, windowWidth );
    EXPECT_EQ( summary->bounds.height, windowHeight );
}

// The active window points somewhere outside the client list — the case an
// override-redirect window (which never joins _NET_CLIENT_LIST) produces.
TEST( Screen,
      ActiveWindowSummaryFailsWhenActiveIsNotAManagedClient )
{
    const TestConnection connection{ xvfbDisplay };
    ASSERT_NE( connection.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( connection.get() ), xcbOk );
    const xcb_screen_t* screen_info =
        default_screen( connection.get(), connection.screen_index() );
    ASSERT_NE( screen_info, nullptr );
    static_cast<void>( create_solid_window( connection.get(),
                                            *screen_info,
                                            knownColor,
                                            knownInstance,
                                            knownClass ) );
    set_active_window( connection.get(),
                       screen_info->root,
                       static_cast<xcb_window_t>( anyWindowId ) );
    ASSERT_TRUE( flush_succeeded( connection.get() ) );

    auto screen = grab::Screen::open( xvfbDisplay );
    ASSERT_TRUE( screen.has_value() ) << screen.error().message;
    auto summary = screen->active_window_summary();

    ASSERT_FALSE( summary.has_value() );
    EXPECT_EQ( summary.error().code, grab::ErrorCode::WindowNotFound );
}

TEST( Screen,
      OpenFailsOnBadDisplay )
{
    auto screen = grab::Screen::open( badDisplay );

    ASSERT_FALSE( screen.has_value() );
    EXPECT_EQ( screen.error().code, grab::ErrorCode::DeviceInaccessible );
}
