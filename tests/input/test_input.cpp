#include "grab/input.hpp"
#include "grab/result.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <array>
#include <chrono>
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

    constexpr const char*      kXvfbDisplay       = ":94";
    constexpr const char*      kUsLayout          = "us";
    constexpr int              kXcbOk             = 0;
    constexpr std::int16_t     kWindowX           = 160;
    constexpr std::int16_t     kWindowY           = 180;
    constexpr std::uint16_t    kWindowWidth       = 320U;
    constexpr std::uint16_t    kWindowHeight      = 220U;
    constexpr std::uint16_t    kWindowBorderWidth = 0U;
    constexpr std::int16_t     kDragFromX         = kWindowX + 64;
    constexpr std::int16_t     kDragFromY         = kWindowY + 68;
    constexpr std::int16_t     kDragToX           = kWindowX + 244;
    constexpr std::int16_t     kDragToY           = kWindowY + 148;
    constexpr std::uint32_t    kWindowValueMask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    constexpr std::uint32_t    kPropertyReplaceMode     = XCB_PROP_MODE_REPLACE;
    constexpr std::uint8_t     kFormat8Bits             = 8U;
    constexpr std::uint8_t     kResponseTypeMask        = 0X7FU;
    constexpr std::uint8_t     kLeftButton              = 1U;
    constexpr std::size_t      kExpectedTypedKeyPresses = 2U;
    constexpr std::int32_t     kMinimumDragMotionEvents = 1;
    constexpr double           kWindowCenterFraction    = 0.5;
    constexpr auto             kEventTimeout            = std::chrono::seconds{ 3 };
    constexpr auto             kEventPollInterval       = std::chrono::milliseconds{ 5 };
    constexpr std::string_view kTypedText               = "Ab";
    constexpr std::string_view kInputInstance           = "grab-input-instance";
    constexpr std::string_view kInputClass              = "GrabInputTestClass";
    constexpr std::string_view kWindowTitle             = "grab input test";
    constexpr std::string_view kMissingClickEvent       = "ButtonPress was not observed";
    constexpr std::string_view kMissingDragSequence =
        "ButtonPress, MotionNotify..., ButtonRelease sequence was not observed";

    enum class ObservedPointerEvent : std::uint8_t
    {
        motion,
        button_press,
        button_release,
    };

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
            xcb_connection_has_error( connection ) != kXcbOk )
        {
            return testing::AssertionFailure() << "xcb_flush failed";
        }
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
    set_title( xcb_connection_t* connection,
               xcb_window_t      window,
               std::string_view  title )
    {
        EXPECT_TRUE( request_succeeded(
            connection,
            xcb_change_property_checked( connection,
                                         kPropertyReplaceMode,
                                         window,
                                         XCB_ATOM_WM_NAME,
                                         XCB_ATOM_STRING,
                                         kFormat8Bits,
                                         static_cast<std::uint32_t>( title.size() ),
                                         title.data() )
        ) );
    }

    [[nodiscard]]
    xcb_window_t
    create_observer_window( xcb_connection_t*   connection,
                            const xcb_screen_t& screen,
                            std::uint32_t       event_mask )
    {
        const xcb_window_t                 window = xcb_generate_id( connection );
        const std::array<std::uint32_t, 2> values{
            screen.black_pixel,
            event_mask,
        };
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
        set_wm_class( connection, window, kInputInstance, kInputClass );
        set_title( connection, window, kWindowTitle );
        EXPECT_TRUE( request_succeeded( connection,
                                        xcb_map_window_checked( connection, window ) ) );
        EXPECT_TRUE( flush_succeeded( connection ) );
        return window;
    }

    void
    focus_window( xcb_connection_t* connection,
                  xcb_window_t      window )
    {
        EXPECT_TRUE(
            request_succeeded( connection,
                               xcb_set_input_focus_checked( connection,
                                                            XCB_INPUT_FOCUS_POINTER_ROOT,
                                                            window,
                                                            XCB_CURRENT_TIME ) )
        );
        EXPECT_TRUE( flush_succeeded( connection ) );
    }

    [[nodiscard]]
    std::size_t
    count_key_presses( xcb_connection_t* connection,
                       std::size_t       expected_count )
    {
        std::size_t key_presses = 0U;
        const auto  deadline    = std::chrono::steady_clock::now() + kEventTimeout;
        while( std::chrono::steady_clock::now() < deadline )
        {
            const auto event = take_xcb_owned( xcb_poll_for_event( connection ) );
            if( event != nullptr )
            {
                const auto response_type =
                    static_cast<std::uint8_t>( event->response_type &
                                               kResponseTypeMask );
                if( response_type == XCB_KEY_PRESS )
                {
                    ++key_presses;
                    if( key_presses >= expected_count )
                    {
                        return key_presses;
                    }
                }
                continue;
            }

            if( xcb_connection_has_error( connection ) != kXcbOk )
            {
                return key_presses;
            }
            std::this_thread::sleep_for( kEventPollInterval );
        }
        return key_presses;
    }

    [[nodiscard]]
    bool
    wait_for_button_press( xcb_connection_t* connection )
    {
        const auto deadline = std::chrono::steady_clock::now() + kEventTimeout;
        while( std::chrono::steady_clock::now() < deadline )
        {
            const auto event = take_xcb_owned( xcb_poll_for_event( connection ) );
            if( event != nullptr )
            {
                const auto response_type =
                    static_cast<std::uint8_t>( event->response_type &
                                               kResponseTypeMask );
                if( response_type == XCB_BUTTON_PRESS )
                {
                    return true;
                }
                continue;
            }

            if( xcb_connection_has_error( connection ) != kXcbOk )
            {
                return false;
            }
            std::this_thread::sleep_for( kEventPollInterval );
        }
        return false;
    }

    [[nodiscard]]
    std::vector<ObservedPointerEvent>
    collect_pointer_events( xcb_connection_t* connection )
    {
        std::vector<ObservedPointerEvent> events;
        const auto deadline = std::chrono::steady_clock::now() + kEventTimeout;
        while( std::chrono::steady_clock::now() < deadline )
        {
            const auto event = take_xcb_owned( xcb_poll_for_event( connection ) );
            if( event != nullptr )
            {
                const auto response_type =
                    static_cast<std::uint8_t>( event->response_type &
                                               kResponseTypeMask );
                if( response_type == XCB_MOTION_NOTIFY )
                {
                    events.push_back( ObservedPointerEvent::motion );
                }
                else if( response_type == XCB_BUTTON_PRESS )
                {
                    events.push_back( ObservedPointerEvent::button_press );
                }
                else if( response_type == XCB_BUTTON_RELEASE )
                {
                    events.push_back( ObservedPointerEvent::button_release );
                    return events;
                }
                continue;
            }

            if( xcb_connection_has_error( connection ) != kXcbOk )
            {
                return events;
            }
            std::this_thread::sleep_for( kEventPollInterval );
        }
        return events;
    }

    [[nodiscard]]
    testing::AssertionResult
    contains_drag_sequence( const std::vector<ObservedPointerEvent>& events )
    {
        bool         saw_press    = false;
        std::int32_t motion_count = 0;
        for( const ObservedPointerEvent event : events )
        {
            if( !saw_press )
            {
                saw_press = event == ObservedPointerEvent::button_press;
                continue;
            }

            if( event == ObservedPointerEvent::motion )
            {
                ++motion_count;
                continue;
            }

            if( event == ObservedPointerEvent::button_release )
            {
                if( motion_count >= kMinimumDragMotionEvents )
                {
                    return testing::AssertionSuccess();
                }
                return testing::AssertionFailure()
                    << "only observed " << motion_count
                    << " MotionNotify events after ButtonPress";
            }
        }
        return testing::AssertionFailure() << kMissingDragSequence;
    }

}    // namespace

TEST( Input,
      TypeTextReachesWindow )
{
    const ObserverConnection observer{ kXvfbDisplay };
    ASSERT_NE( observer.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( observer.get() ), kXcbOk );
    const xcb_screen_t* screen =
        default_screen( observer.get(), observer.screen_index() );
    ASSERT_NE( screen, nullptr );
    const xcb_window_t window =
        create_observer_window( observer.get(), *screen, XCB_EVENT_MASK_KEY_PRESS );
    focus_window( observer.get(), window );

    auto input = grab::Input::open( kXvfbDisplay, kUsLayout );
    ASSERT_TRUE( input.has_value() ) << input.error().message;

    const auto type_result = input->type_text( kTypedText );
    ASSERT_TRUE( type_result.has_value() ) << type_result.error().message;

    EXPECT_GE( count_key_presses( observer.get(), kExpectedTypedKeyPresses ),
               kExpectedTypedKeyPresses );
}

TEST( Input,
      ClickInWindowHitsInside )
{
    const ObserverConnection observer{ kXvfbDisplay };
    ASSERT_NE( observer.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( observer.get() ), kXcbOk );
    const xcb_screen_t* screen =
        default_screen( observer.get(), observer.screen_index() );
    ASSERT_NE( screen, nullptr );
    static_cast<void>(
        create_observer_window( observer.get(), *screen, XCB_EVENT_MASK_BUTTON_PRESS )
    );

    auto input = grab::Input::open( kXvfbDisplay, kUsLayout );
    ASSERT_TRUE( input.has_value() ) << input.error().message;
    auto located = input->locate( { std::string{ kInputClass } }, kWindowTitle );
    ASSERT_TRUE( located.has_value() ) << located.error().message;

    const auto click_result = input->click_in_window( *located,
                                                      kWindowCenterFraction,
                                                      kWindowCenterFraction,
                                                      kLeftButton );
    ASSERT_TRUE( click_result.has_value() ) << click_result.error().message;

    EXPECT_TRUE( wait_for_button_press( observer.get() ) ) << kMissingClickEvent;
}

TEST( Input,
      DragEmitsSequence )
{
    const ObserverConnection observer{ kXvfbDisplay };
    ASSERT_NE( observer.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( observer.get() ), kXcbOk );
    const xcb_screen_t* screen =
        default_screen( observer.get(), observer.screen_index() );
    ASSERT_NE( screen, nullptr );
    static_cast<void>( create_observer_window( observer.get(),
                                               *screen,
                                               XCB_EVENT_MASK_BUTTON_PRESS |
                                                   XCB_EVENT_MASK_BUTTON_RELEASE |
                                                   XCB_EVENT_MASK_POINTER_MOTION ) );

    auto input = grab::Input::open( kXvfbDisplay, kUsLayout );
    ASSERT_TRUE( input.has_value() ) << input.error().message;

    const auto drag_result = input->drag( { .x = kDragFromX, .y = kDragFromY },
                                          { .x = kDragToX, .y = kDragToY } );
    ASSERT_TRUE( drag_result.has_value() ) << drag_result.error().message;

    const std::vector<ObservedPointerEvent> events =
        collect_pointer_events( observer.get() );
    EXPECT_TRUE( contains_drag_sequence( events ) );
}
