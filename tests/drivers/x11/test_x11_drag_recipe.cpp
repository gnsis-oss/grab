#include "drivers/desktop/x11/x11_drag_recipe.hpp"
#include "drivers/desktop/x11/x11_xtest_seat.hpp"
#include "grab/drag.hpp"
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
#include <vector>
#include <xcb/xcb.h>
#include <xcb/xproto.h>
// clang-format on

namespace
{

    constexpr const char*      xvfbDisplay             = ":96";
    constexpr int              xcbOk                   = 0;
    constexpr std::int16_t     windowX                 = 120;
    constexpr std::int16_t     windowY                 = 140;
    constexpr std::uint16_t    windowWidth             = 420U;
    constexpr std::uint16_t    windowHeight            = 280U;
    constexpr std::uint16_t    windowBorderWidth       = 0U;
    constexpr std::int16_t     dragFromX               = windowX + 64;
    constexpr std::int16_t     dragFromY               = windowY + 72;
    constexpr std::int16_t     dragToX                 = windowX + 312;
    constexpr std::int16_t     dragToY                 = windowY + 184;
    constexpr std::int16_t     menuItemX               = windowX + 180;
    constexpr std::int16_t     menuItemY               = windowY + 96;
    constexpr std::int32_t     interpolationSteps      = 16;
    constexpr std::int32_t     minimumDragMotionEvents = 1;
    constexpr auto             stepDwell               = std::chrono::milliseconds{ 2 };
    constexpr std::uint32_t    windowEventMask         = XCB_EVENT_MASK_BUTTON_PRESS |
                                                         XCB_EVENT_MASK_BUTTON_RELEASE |
                                                         XCB_EVENT_MASK_POINTER_MOTION;
    constexpr std::uint32_t    windowValueMask  = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    constexpr std::uint8_t     responseTypeMask = 0X7FU;
    constexpr auto             pointerEventTimeout = std::chrono::seconds{ 2 };
    constexpr auto             eventPollInterval   = std::chrono::milliseconds{ 5 };
    constexpr std::string_view missingDragSequence =
        "ButtonPress, MotionNotify..., ButtonRelease sequence was not observed";
    constexpr std::string_view missingClickSequence =
        "ButtonPress, ButtonRelease sequence was not observed";

    enum class ObservedEvent : std::uint8_t
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
    xcb_window_t
    create_observer_window( xcb_connection_t*   connection,
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

    [[nodiscard]]
    std::vector<ObservedEvent>
    collect_pointer_events( xcb_connection_t* connection )
    {
        std::vector<ObservedEvent> events;
        const auto deadline = std::chrono::steady_clock::now() + pointerEventTimeout;
        while( std::chrono::steady_clock::now() < deadline )
        {
            const auto event = take_xcb_owned( xcb_poll_for_event( connection ) );
            if( event != nullptr )
            {
                const auto response_type =
                    static_cast<std::uint8_t>( event->response_type & responseTypeMask );
                if( response_type == XCB_MOTION_NOTIFY )
                {
                    events.push_back( ObservedEvent::Motion );
                }
                else if( response_type == XCB_BUTTON_PRESS )
                {
                    events.push_back( ObservedEvent::ButtonPress );
                }
                else if( response_type == XCB_BUTTON_RELEASE )
                {
                    events.push_back( ObservedEvent::ButtonRelease );
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
    contains_drag_sequence( const std::vector<ObservedEvent>& events,
                            std::int32_t                      minimum_motion_count )
    {
        bool         saw_press    = false;
        std::int32_t motion_count = 0;
        for( const ObservedEvent event : events )
        {
            if( !saw_press )
            {
                saw_press = event == ObservedEvent::ButtonPress;
                continue;
            }

            if( event == ObservedEvent::Motion )
            {
                ++motion_count;
                continue;
            }

            if( event == ObservedEvent::ButtonRelease )
            {
                if( motion_count >= minimum_motion_count )
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

    [[nodiscard]]
    testing::AssertionResult
    contains_click_sequence( const std::vector<ObservedEvent>& events )
    {
        bool saw_press = false;
        for( const ObservedEvent event : events )
        {
            if( !saw_press )
            {
                saw_press = event == ObservedEvent::ButtonPress;
                continue;
            }

            if( event == ObservedEvent::ButtonRelease )
            {
                return testing::AssertionSuccess();
            }
        }
        return testing::AssertionFailure() << missingClickSequence;
    }

}    // namespace

TEST( DragOptions,
      DefaultsAreVisibleAndOverridable )
{
    constexpr std::int32_t             customInterpolationSteps = 4;
    constexpr auto                     customStepDwell = std::chrono::milliseconds{ 3 };

    constexpr grab::input::DragOptions defaults;
    EXPECT_EQ( defaults.interpolation_steps,
               grab::input::DragOptions::defaultInterpolationSteps );
    EXPECT_EQ( defaults.step_dwell, grab::input::DragOptions::defaultStepDwell );
    EXPECT_EQ( defaults.interpolation_steps, 16 );
    EXPECT_EQ( defaults.step_dwell, std::chrono::milliseconds{ 8 } );
    EXPECT_EQ( defaults.path, grab::input::DragOptions::Path::Linear );

    constexpr grab::input::DragOptions customized{
        .interpolation_steps = customInterpolationSteps,
        .step_dwell          = customStepDwell,
    };
    EXPECT_EQ( customized.interpolation_steps, customInterpolationSteps );
    EXPECT_EQ( customized.step_dwell, customStepDwell );
}

TEST( Gestures,
      LinearDragEmitsPressMotionsRelease )
{
    const ObserverConnection observer{ xvfbDisplay };
    ASSERT_NE( observer.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( observer.get() ), xcbOk );
    const xcb_screen_t* screen =
        default_screen( observer.get(), observer.screen_index() );
    ASSERT_NE( screen, nullptr );
    static_cast<void>( create_observer_window( observer.get(), *screen ) );

    auto seat = grab::input::Seat::open( xvfbDisplay );
    ASSERT_TRUE( seat.has_value() ) << seat.error().message;
    const grab::input::DragOptions options{
        .interpolation_steps = interpolationSteps,
        .step_dwell          = stepDwell,
    };

    const auto drag_result =
        grab::drivers::desktop::x11::execute_drag( *seat,
                                                   { .x = dragFromX, .y = dragFromY },
                                                   { .x = dragToX, .y = dragToY },
                                                   options );
    ASSERT_TRUE( drag_result.has_value() ) << drag_result.error().message;

    const std::vector<ObservedEvent> events = collect_pointer_events( observer.get() );
    EXPECT_TRUE( contains_drag_sequence( events, interpolationSteps ) );

    const auto pointer = take_xcb_owned(
        xcb_query_pointer_reply( observer.get(),
                                 xcb_query_pointer( observer.get(), screen->root ),
                                 nullptr )
    );
    ASSERT_NE( pointer, nullptr );
    EXPECT_EQ( pointer->root_x, dragToX );
    EXPECT_EQ( pointer->root_y, dragToY );
}

TEST( Gestures,
      CurveDragEndsAtRequestedPoint )
{
    const ObserverConnection observer{ xvfbDisplay };
    ASSERT_NE( observer.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( observer.get() ), xcbOk );
    const xcb_screen_t* screen =
        default_screen( observer.get(), observer.screen_index() );
    ASSERT_NE( screen, nullptr );
    static_cast<void>( create_observer_window( observer.get(), *screen ) );

    auto seat = grab::input::Seat::open( xvfbDisplay );
    ASSERT_TRUE( seat.has_value() ) << seat.error().message;
    const grab::input::DragOptions options{
        .interpolation_steps = interpolationSteps,
        .step_dwell          = stepDwell,
        .path                = grab::input::DragOptions::Path::Cubic,
    };

    const auto drag_result =
        grab::drivers::desktop::x11::execute_drag( *seat,
                                                   { .x = dragFromX, .y = dragFromY },
                                                   { .x = dragToX, .y = dragToY },
                                                   options );
    ASSERT_TRUE( drag_result.has_value() ) << drag_result.error().message;

    const std::vector<ObservedEvent> events = collect_pointer_events( observer.get() );
    EXPECT_TRUE( contains_drag_sequence( events, minimumDragMotionEvents ) );

    const auto pointer = take_xcb_owned(
        xcb_query_pointer_reply( observer.get(),
                                 xcb_query_pointer( observer.get(), screen->root ),
                                 nullptr )
    );
    ASSERT_NE( pointer, nullptr );
    EXPECT_EQ( pointer->root_x, dragToX );
    EXPECT_EQ( pointer->root_y, dragToY );
}

TEST( Gestures,
      MenuClickEmitsClickAtPoint )
{
    const ObserverConnection observer{ xvfbDisplay };
    ASSERT_NE( observer.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( observer.get() ), xcbOk );
    const xcb_screen_t* screen =
        default_screen( observer.get(), observer.screen_index() );
    ASSERT_NE( screen, nullptr );
    static_cast<void>( create_observer_window( observer.get(), *screen ) );

    auto seat = grab::input::Seat::open( xvfbDisplay );
    ASSERT_TRUE( seat.has_value() ) << seat.error().message;

    const grab::input::DragOptions click_options{ .interpolation_steps = 1 };
    const auto                     click_result =
        grab::drivers::desktop::x11::execute_drag( *seat,
                                                   { .x = menuItemX, .y = menuItemY },
                                                   { .x = menuItemX, .y = menuItemY },
                                                   click_options );
    ASSERT_TRUE( click_result.has_value() ) << click_result.error().message;

    const std::vector<ObservedEvent> events = collect_pointer_events( observer.get() );
    EXPECT_TRUE( contains_click_sequence( events ) );
}
