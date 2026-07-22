#include "drivers/desktop/x11/x11_xtest_seat.hpp"
#include "grab/result.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <thread>
#include <xcb/xcb.h>
#include <xcb/xproto.h>
// clang-format on

namespace
{

    constexpr const char*   xvfbDisplay            = ":99";
    constexpr const char*   badDisplay             = ":name-that-does-not-exist-999";
    constexpr auto          deviceInaccessibleCode = grab::ErrorCode::DeviceInaccessible;
    constexpr int           xcbOk                  = 0;
    constexpr std::int16_t  targetX                = 321;
    constexpr std::int16_t  targetY                = 654;
    constexpr std::int16_t  windowX                = 100;
    constexpr std::int16_t  windowY                = 120;
    constexpr std::uint16_t windowWidth            = 160U;
    constexpr std::uint16_t windowHeight           = 120U;
    constexpr std::int16_t  windowHalfWidth        = 80;
    constexpr std::int16_t  windowHalfHeight       = 60;
    constexpr std::int16_t  windowCenterX          = windowX + windowHalfWidth;
    constexpr std::int16_t  windowCenterY          = windowY + windowHalfHeight;
    constexpr std::uint16_t windowBorderWidth      = 0U;
    constexpr std::uint32_t windowEventMask =
        XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE;
    constexpr std::uint32_t    windowValueMask  = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    constexpr std::uint8_t     leftButton       = 1U;
    constexpr std::uint8_t     responseTypeMask = 0X7FU;
    constexpr auto             buttonEventTimeout      = std::chrono::seconds{ 2 };
    constexpr auto             buttonEventPollInterval = std::chrono::milliseconds{ 10 };
    constexpr std::string_view pointerQueryFailed      = "xcb_query_pointer failed";
    constexpr std::string_view buttonPressWasNotEmitted =
        "button press was not observed";

    template<typename T>
    using XcbOwned = std::unique_ptr<T, decltype( &std::free )>;

    template<typename T>
    [[nodiscard]]
    XcbOwned<T>
    take_xcb_owned( T* pointer ) noexcept
    {
        return XcbOwned<T>{ pointer, &std::free };
    }

    class ObserverConnection
    {
        public:

            explicit ObserverConnection( const char* display ) :
                connection_( xcb_connect( display,
                                          &screen_index_ ) )
            {
            }

            ~ObserverConnection()
            {
                if( connection_ != nullptr )
                {
                    xcb_disconnect( connection_ );
                }
            }

            ObserverConnection( const ObserverConnection& ) = delete;
            ObserverConnection&
            operator=( const ObserverConnection& )     = delete;
            ObserverConnection( ObserverConnection&& ) = delete;
            ObserverConnection&
            operator=( ObserverConnection&& ) = delete;

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
    bool
    wait_for_button_press( xcb_connection_t* connection )
    {
        const auto deadline = std::chrono::steady_clock::now() + buttonEventTimeout;
        while( std::chrono::steady_clock::now() < deadline )
        {
            const auto event = take_xcb_owned( xcb_poll_for_event( connection ) );
            if( event != nullptr )
            {
                const auto response_type =
                    static_cast<std::uint8_t>( event->response_type & responseTypeMask );
                if( response_type == XCB_BUTTON_PRESS )
                {
                    return true;
                }
                continue;
            }

            if( xcb_connection_has_error( connection ) != xcbOk )
            {
                return false;
            }
            std::this_thread::sleep_for( buttonEventPollInterval );
        }
        return false;
    }

    [[nodiscard]]
    xcb_window_t
    create_button_window( xcb_connection_t*   connection,
                          const xcb_screen_t& screen )
    {
        const xcb_window_t                 window = xcb_generate_id( connection );
        const std::array<std::uint32_t, 2> values{
            screen.black_pixel,
            windowEventMask,
        };
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
        EXPECT_TRUE( request_succeeded( connection,
                                        xcb_map_window_checked( connection, window ) ) );
        EXPECT_TRUE( flush_succeeded( connection ) );
        return window;
    }

}    // namespace

TEST( Seat,
      OpenConnectsAndHasXtest )
{
    auto seat = grab::input::Seat::open( xvfbDisplay );

    ASSERT_TRUE( seat.has_value() ) << seat.error().message;
}

TEST( Seat,
      PointerMoveIsObservable )
{
    const ObserverConnection observer{ xvfbDisplay };
    ASSERT_NE( observer.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( observer.get() ), xcbOk );
    const xcb_screen_t* screen =
        default_screen( observer.get(), observer.screen_index() );
    ASSERT_NE( screen, nullptr );

    auto seat = grab::input::Seat::open( xvfbDisplay );
    ASSERT_TRUE( seat.has_value() ) << seat.error().message;
    ASSERT_TRUE( seat->move_pointer_absolute( targetX, targetY ).has_value() );
    ASSERT_TRUE( seat->flush().has_value() );

    xcb_generic_error_t* raw_error = nullptr;
    const auto           pointer   = take_xcb_owned(
        xcb_query_pointer_reply( observer.get(),
                                 xcb_query_pointer( observer.get(), screen->root ),
                                 &raw_error )
    );
    const auto error = take_xcb_owned( raw_error );
    ASSERT_EQ( error, nullptr ) << pointerQueryFailed;
    ASSERT_NE( pointer, nullptr ) << pointerQueryFailed;
    EXPECT_EQ( pointer->root_x, targetX );
    EXPECT_EQ( pointer->root_y, targetY );
}

TEST( Seat,
      ButtonPressReachesWindow )
{
    const ObserverConnection observer{ xvfbDisplay };
    ASSERT_NE( observer.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( observer.get() ), xcbOk );
    const xcb_screen_t* screen =
        default_screen( observer.get(), observer.screen_index() );
    ASSERT_NE( screen, nullptr );
    static_cast<void>( create_button_window( observer.get(), *screen ) );

    auto seat = grab::input::Seat::open( xvfbDisplay );
    ASSERT_TRUE( seat.has_value() ) << seat.error().message;
    ASSERT_TRUE(
        seat->move_pointer_absolute( windowCenterX, windowCenterY ).has_value()
    );
    ASSERT_TRUE( seat->flush().has_value() );
    ASSERT_TRUE( seat->button( leftButton, true ).has_value() );
    ASSERT_TRUE( seat->button( leftButton, false ).has_value() );
    ASSERT_TRUE( seat->flush().has_value() );

    EXPECT_TRUE( wait_for_button_press( observer.get() ) ) << buttonPressWasNotEmitted;
}

TEST( Seat,
      OpenFailsOnBadDisplay )
{
    auto seat = grab::input::Seat::open( badDisplay );

    ASSERT_FALSE( seat.has_value() );
    EXPECT_EQ( seat.error().code, deviceInaccessibleCode );
}
