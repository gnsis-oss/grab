#include "grab/result.hpp"
#include "input/gestures.hpp"
#include "input/seat.hpp"

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

    constexpr const char*      kXvfbDisplay        = ":96";
    constexpr int              kXcbOk              = 0;
    constexpr std::int16_t     kWindowX            = 120;
    constexpr std::int16_t     kWindowY            = 140;
    constexpr std::uint16_t    kWindowWidth        = 420U;
    constexpr std::uint16_t    kWindowHeight       = 280U;
    constexpr std::uint16_t    kWindowBorderWidth  = 0U;
    constexpr std::int16_t     kDragFromX          = kWindowX + 64;
    constexpr std::int16_t     kDragFromY          = kWindowY + 72;
    constexpr std::int16_t     kDragToX            = kWindowX + 312;
    constexpr std::int16_t     kDragToY            = kWindowY + 184;
    constexpr std::int16_t     kMenuItemX          = kWindowX + 180;
    constexpr std::int16_t     kMenuItemY          = kWindowY + 96;
    constexpr std::int32_t     kInterpolationSteps = 16;
    constexpr auto             kStepDwell          = std::chrono::milliseconds{ 2 };
    constexpr auto             kDragStartDwell     = std::chrono::milliseconds{ 2 };
    constexpr std::uint32_t    kWindowEventMask    = XCB_EVENT_MASK_BUTTON_PRESS |
                                                     XCB_EVENT_MASK_BUTTON_RELEASE |
                                                     XCB_EVENT_MASK_POINTER_MOTION;
    constexpr std::uint32_t    kWindowValueMask  = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    constexpr std::uint8_t     kResponseTypeMask = 0X7FU;
    constexpr auto             kPointerEventTimeout = std::chrono::seconds{ 2 };
    constexpr auto             kEventPollInterval   = std::chrono::milliseconds{ 5 };
    constexpr std::string_view kMissingDragSequence =
        "ButtonPress, MotionNotify..., ButtonRelease sequence was not observed";
    constexpr std::string_view kMissingClickSequence =
        "ButtonPress, ButtonRelease sequence was not observed";

    enum class ObservedEvent : std::uint8_t
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
    xcb_window_t
    create_observer_window( xcb_connection_t*   connection,
                            const xcb_screen_t& screen )
    {
        const xcb_window_t                 window = xcb_generate_id( connection );
        const std::array<std::uint32_t, 2> values{
            screen.black_pixel,
            kWindowEventMask,
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
        const auto deadline = std::chrono::steady_clock::now() + kPointerEventTimeout;
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
                    events.push_back( ObservedEvent::motion );
                }
                else if( response_type == XCB_BUTTON_PRESS )
                {
                    events.push_back( ObservedEvent::button_press );
                }
                else if( response_type == XCB_BUTTON_RELEASE )
                {
                    events.push_back( ObservedEvent::button_release );
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
    contains_drag_sequence( const std::vector<ObservedEvent>& events,
                            std::int32_t                      minimum_motion_count )
    {
        bool         saw_press    = false;
        std::int32_t motion_count = 0;
        for( const ObservedEvent event : events )
        {
            if( !saw_press )
            {
                saw_press = event == ObservedEvent::button_press;
                continue;
            }

            if( event == ObservedEvent::motion )
            {
                ++motion_count;
                continue;
            }

            if( event == ObservedEvent::button_release )
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
        return testing::AssertionFailure() << kMissingDragSequence;
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
                saw_press = event == ObservedEvent::button_press;
                continue;
            }

            if( event == ObservedEvent::button_release )
            {
                return testing::AssertionSuccess();
            }
        }
        return testing::AssertionFailure() << kMissingClickSequence;
    }

}    // namespace

TEST( Gestures,
      QtDragEmitsPressMotionsRelease )
{
    const ObserverConnection observer{ kXvfbDisplay };
    ASSERT_NE( observer.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( observer.get() ), kXcbOk );
    const xcb_screen_t* screen =
        default_screen( observer.get(), observer.screen_index() );
    ASSERT_NE( screen, nullptr );
    static_cast<void>( create_observer_window( observer.get(), *screen ) );

    auto seat = grab::input::Seat::open( kXvfbDisplay );
    ASSERT_TRUE( seat.has_value() ) << seat.error().message;
    const grab::input::QtDragParams params{
        .interpolation_steps = kInterpolationSteps,
        .step_dwell          = kStepDwell,
        .drag_start_dwell    = kDragStartDwell,
    };

    const auto drag_result = grab::input::qt_drag( *seat,
                                                   { .x = kDragFromX, .y = kDragFromY },
                                                   { .x = kDragToX, .y = kDragToY },
                                                   params );
    ASSERT_TRUE( drag_result.has_value() ) << drag_result.error().message;

    const std::vector<ObservedEvent> events = collect_pointer_events( observer.get() );
    EXPECT_TRUE( contains_drag_sequence( events, kInterpolationSteps ) );
}

TEST( Gestures,
      MenuClickEmitsClickAtPoint )
{
    const ObserverConnection observer{ kXvfbDisplay };
    ASSERT_NE( observer.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( observer.get() ), kXcbOk );
    const xcb_screen_t* screen =
        default_screen( observer.get(), observer.screen_index() );
    ASSERT_NE( screen, nullptr );
    static_cast<void>( create_observer_window( observer.get(), *screen ) );

    auto seat = grab::input::Seat::open( kXvfbDisplay );
    ASSERT_TRUE( seat.has_value() ) << seat.error().message;

    const auto click_result =
        grab::input::menu_click( *seat, { .x = kMenuItemX, .y = kMenuItemY } );
    ASSERT_TRUE( click_result.has_value() ) << click_result.error().message;

    const std::vector<ObservedEvent> events = collect_pointer_events( observer.get() );
    EXPECT_TRUE( contains_click_sequence( events ) );
}
