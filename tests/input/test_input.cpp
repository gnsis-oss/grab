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

    constexpr const char*      xvfbDisplay       = ":94";
    constexpr const char*      usLayout          = "us";
    constexpr int              xcbOk             = 0;
    constexpr std::int16_t     windowX           = 160;
    constexpr std::int16_t     windowY           = 180;
    constexpr std::uint16_t    windowWidth       = 320U;
    constexpr std::uint16_t    windowHeight      = 220U;
    constexpr std::uint16_t    windowBorderWidth = 0U;
    constexpr std::int16_t     dragFromX         = windowX + 64;
    constexpr std::int16_t     dragFromY         = windowY + 68;
    constexpr std::int16_t     dragToX           = windowX + 244;
    constexpr std::int16_t     dragToY           = windowY + 148;
    constexpr std::uint32_t    windowValueMask   = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    constexpr std::uint32_t    propertyReplaceMode     = XCB_PROP_MODE_REPLACE;
    constexpr std::uint8_t     format8Bits             = 8U;
    constexpr std::uint8_t     responseTypeMask        = 0X7FU;
    constexpr std::size_t      expectedTypedKeyPresses = 2U;
    constexpr std::size_t      expectedPressedKeyCount = 1U;
    constexpr std::int32_t     minimumDragMotionEvents = 1;
    constexpr auto             eventTimeout            = std::chrono::seconds{ 3 };
    constexpr auto             eventPollInterval       = std::chrono::milliseconds{ 5 };
    constexpr std::string_view typedText               = "Ab";
    constexpr std::string_view returnKey               = "Return";
    constexpr std::string_view unknownKey              = "NoSuchGrabKey";
    constexpr std::string_view inputInstance           = "grab-input-instance";
    constexpr std::string_view inputClass              = "GrabInputTestClass";
    constexpr std::string_view windowTitle             = "grab input test";
    constexpr std::string_view missingDragSequence =
        "ButtonPress, MotionNotify..., ButtonRelease sequence was not observed";

    enum class ObservedPointerEvent : std::uint8_t
    {
        Motion,
        ButtonPress,
        ButtonRelease,
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
            xcb_connection_has_error( connection ) != xcbOk )
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
    set_title( xcb_connection_t* connection,
               xcb_window_t      window,
               std::string_view  title )
    {
        EXPECT_TRUE( request_succeeded(
            connection,
            xcb_change_property_checked( connection,
                                         propertyReplaceMode,
                                         window,
                                         XCB_ATOM_WM_NAME,
                                         XCB_ATOM_STRING,
                                         format8Bits,
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
        set_wm_class( connection, window, inputInstance, inputClass );
        set_title( connection, window, windowTitle );
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
        const auto  deadline    = std::chrono::steady_clock::now() + eventTimeout;
        while( std::chrono::steady_clock::now() < deadline )
        {
            const auto event = take_xcb_owned( xcb_poll_for_event( connection ) );
            if( event != nullptr )
            {
                const auto response_type =
                    static_cast<std::uint8_t>( event->response_type & responseTypeMask );
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

            if( xcb_connection_has_error( connection ) != xcbOk )
            {
                return key_presses;
            }
            std::this_thread::sleep_for( eventPollInterval );
        }
        return key_presses;
    }

    [[nodiscard]]
    std::vector<ObservedPointerEvent>
    collect_pointer_events( xcb_connection_t* connection )
    {
        std::vector<ObservedPointerEvent> events;
        const auto deadline = std::chrono::steady_clock::now() + eventTimeout;
        while( std::chrono::steady_clock::now() < deadline )
        {
            const auto event = take_xcb_owned( xcb_poll_for_event( connection ) );
            if( event != nullptr )
            {
                const auto response_type =
                    static_cast<std::uint8_t>( event->response_type & responseTypeMask );
                if( response_type == XCB_MOTION_NOTIFY )
                {
                    events.push_back( ObservedPointerEvent::Motion );
                }
                else if( response_type == XCB_BUTTON_PRESS )
                {
                    events.push_back( ObservedPointerEvent::ButtonPress );
                }
                else if( response_type == XCB_BUTTON_RELEASE )
                {
                    events.push_back( ObservedPointerEvent::ButtonRelease );
                    return events;
                }
                continue;
            }

            if( xcb_connection_has_error( connection ) != xcbOk )
            {
                return events;
            }
            std::this_thread::sleep_for( eventPollInterval );
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
                saw_press = event == ObservedPointerEvent::ButtonPress;
                continue;
            }

            if( event == ObservedPointerEvent::Motion )
            {
                ++motion_count;
                continue;
            }

            if( event == ObservedPointerEvent::ButtonRelease )
            {
                if( motion_count >= minimumDragMotionEvents )
                {
                    return testing::AssertionSuccess();
                }
                return testing::AssertionFailure()
                    << "only observed " << motion_count
                    << " MotionNotify events after ButtonPress";
            }
        }
        return testing::AssertionFailure() << missingDragSequence;
    }

}    // namespace

TEST( Input,
      TypeTextReachesWindow )
{
    const ObserverConnection observer{ xvfbDisplay };
    ASSERT_NE( observer.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( observer.get() ), xcbOk );
    const xcb_screen_t* screen =
        default_screen( observer.get(), observer.screen_index() );
    ASSERT_NE( screen, nullptr );
    const xcb_window_t window =
        create_observer_window( observer.get(), *screen, XCB_EVENT_MASK_KEY_PRESS );
    focus_window( observer.get(), window );

    auto input = grab::Input::open( xvfbDisplay );
    ASSERT_TRUE( input.has_value() ) << input.error().message;

    const auto type_result = input->type_text( typedText );
    ASSERT_TRUE( type_result.has_value() ) << type_result.error().message;

    EXPECT_GE( count_key_presses( observer.get(), expectedTypedKeyPresses ),
               expectedTypedKeyPresses );
}

TEST( Input,
      PressKeyReachesWindow )
{
    const ObserverConnection observer{ xvfbDisplay };
    ASSERT_NE( observer.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( observer.get() ), xcbOk );
    const xcb_screen_t* screen =
        default_screen( observer.get(), observer.screen_index() );
    ASSERT_NE( screen, nullptr );
    const xcb_window_t window =
        create_observer_window( observer.get(), *screen, XCB_EVENT_MASK_KEY_PRESS );
    focus_window( observer.get(), window );

    auto input = grab::Input::open( xvfbDisplay, usLayout );
    ASSERT_TRUE( input.has_value() ) << input.error().message;

    const auto key_result = input->press_key( returnKey );
    ASSERT_TRUE( key_result.has_value() ) << key_result.error().message;

    EXPECT_GE( count_key_presses( observer.get(), expectedPressedKeyCount ),
               expectedPressedKeyCount );
}

TEST( Input,
      PressKeyRejectsUnknownName )
{
    auto input = grab::Input::open( xvfbDisplay );
    ASSERT_TRUE( input.has_value() ) << input.error().message;

    const auto key_result = input->press_key( unknownKey );

    ASSERT_FALSE( key_result.has_value() );
    EXPECT_EQ( key_result.error().code, grab::ErrorCode::UnsupportedCharacter );
}

TEST( Input,
      DragEmitsSequence )
{
    const ObserverConnection observer{ xvfbDisplay };
    ASSERT_NE( observer.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( observer.get() ), xcbOk );
    const xcb_screen_t* screen =
        default_screen( observer.get(), observer.screen_index() );
    ASSERT_NE( screen, nullptr );
    static_cast<void>( create_observer_window( observer.get(),
                                               *screen,
                                               XCB_EVENT_MASK_BUTTON_PRESS |
                                                   XCB_EVENT_MASK_BUTTON_RELEASE |
                                                   XCB_EVENT_MASK_POINTER_MOTION ) );

    auto input = grab::Input::open( xvfbDisplay, usLayout );
    ASSERT_TRUE( input.has_value() ) << input.error().message;

    const auto drag_result = input->drag( { .x = dragFromX, .y = dragFromY },
                                          { .x = dragToX, .y = dragToY } );
    ASSERT_TRUE( drag_result.has_value() ) << drag_result.error().message;

    const std::vector<ObservedPointerEvent> events =
        collect_pointer_events( observer.get() );
    EXPECT_TRUE( contains_drag_sequence( events ) );
}
