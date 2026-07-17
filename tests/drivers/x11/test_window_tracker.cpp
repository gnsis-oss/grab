#include "drivers/desktop/x11/window_tracker.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/result.hpp"
#include "kernel/scheduling/reactor.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <xcb/xcb.h>
#include <xcb/xproto.h>
// clang-format on

namespace
{

    constexpr const char*      xvfbDisplay         = ":95";
    constexpr int              xcbOk               = 0;
    constexpr int              invalidScreenIndex  = 0;
    constexpr std::int16_t     windowX             = 24;
    constexpr std::int16_t     windowY             = 32;
    constexpr std::uint16_t    windowWidth         = 180U;
    constexpr std::uint16_t    windowHeight        = 120U;
    constexpr std::uint16_t    windowBorderWidth   = 0U;
    constexpr std::uint32_t    windowValueMask     = XCB_CW_BACK_PIXEL;
    constexpr std::uint32_t    propertyReplaceMode = XCB_PROP_MODE_REPLACE;
    constexpr std::uint8_t     format8Bits         = 8U;
    constexpr std::uint8_t     format32Bits        = 32U;
    constexpr std::size_t      subscriptionDepth   = 32U;
    constexpr auto             threadReadyTimeout  = std::chrono::seconds{ 2 };
    constexpr auto             registrationTimeout = std::chrono::seconds{ 2 };
    constexpr auto             eventTimeout        = std::chrono::seconds{ 3 };
    constexpr auto             pollInterval        = std::chrono::milliseconds{ 20 };
    constexpr auto             eventPollInterval   = std::chrono::milliseconds{ 10 };

    constexpr std::string_view reactorDidNotStart  = "reactor thread did not start";
    constexpr std::string_view trackerDidNotStart  = "window tracker did not start";
    constexpr std::string_view trackerNotReady = "window tracker fd did not register";
    constexpr std::string_view windowInstance  = "grab-window-tracker-instance";
    constexpr std::string_view windowClass     = "GrabWindowTrackerClass";
    constexpr std::string_view initialTitle    = "grab window tracker initial title";
    constexpr std::string_view changedTitle    = "grab window tracker changed title";

    template<typename T>
    using XcbOwned = std::unique_ptr<T, decltype( &std::free )>;

    template<typename T>
    [[nodiscard]]
    XcbOwned<T>
    take_xcb_owned( T* pointer ) noexcept
    {
        return XcbOwned<T>{ pointer, &std::free };
    }

    class RunningReactor
    {
        public:

            RunningReactor() :
                started_( reactor_started_.get_future() ),
                thread_(
                    [this]
                    {
                        reactor_started_.set_value();
                        result_ = reactor_.run();
                    }
                )
            {
            }

            ~RunningReactor()
            {
                stop_and_join();
            }

            RunningReactor( const RunningReactor& ) = delete;
            RunningReactor&
            operator=( const RunningReactor& ) = delete;
            RunningReactor( RunningReactor&& ) = delete;
            RunningReactor&
            operator=( RunningReactor&& ) = delete;

            [[nodiscard]]
            bool
            wait_until_started()
            {
                return started_.wait_for( threadReadyTimeout ) ==
                       std::future_status::ready;
            }

            [[nodiscard]]
            grab::core::Reactor&
            reactor() noexcept
            {
                return reactor_;
            }

            void
            stop_and_join() noexcept
            {
                reactor_.stop();
                if( thread_.joinable() )
                {
                    thread_.join();
                }
            }

            [[nodiscard]]
            const grab::Result<void>&
            result() const noexcept
            {
                return result_;
            }

        private:

            grab::core::Reactor reactor_;
            std::promise<void>  reactor_started_;
            std::future<void>   started_;
            grab::Result<void>  result_;
            std::thread         thread_;
    };

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
    set_title( xcb_connection_t* connection,
               xcb_window_t      window,
               std::string_view  title )
    {
        xcb_atom_t net_wm_name = XCB_ATOM_NONE;
        xcb_atom_t utf8_string = XCB_ATOM_NONE;
        ASSERT_TRUE( intern_atom( connection, "_NET_WM_NAME", net_wm_name ) );
        ASSERT_TRUE( intern_atom( connection, "UTF8_STRING", utf8_string ) );
        EXPECT_TRUE( request_succeeded(
            connection,
            xcb_change_property_checked( connection,
                                         propertyReplaceMode,
                                         window,
                                         net_wm_name,
                                         utf8_string,
                                         format8Bits,
                                         static_cast<std::uint32_t>( title.size() ),
                                         title.data() )
        ) );
    }

    void
    set_active_window( xcb_connection_t* connection,
                       xcb_window_t      root_window,
                       xcb_window_t      window )
    {
        xcb_atom_t active_window_atom = XCB_ATOM_NONE;
        ASSERT_TRUE(
            intern_atom( connection, "_NET_ACTIVE_WINDOW", active_window_atom )
        );
        const xcb_window_t property_owner = root_window;
        const xcb_atom_t   property_name  = active_window_atom;
        EXPECT_TRUE( request_succeeded( connection,
                                        xcb_change_property_checked( connection,
                                                                     propertyReplaceMode,
                                                                     property_owner,
                                                                     property_name,
                                                                     XCB_ATOM_WINDOW,
                                                                     format32Bits,
                                                                     1U,
                                                                     &window ) ) );
    }

    [[nodiscard]]
    xcb_window_t
    create_window_with_properties( xcb_connection_t*   connection,
                                   const xcb_screen_t& screen,
                                   std::string_view    instance,
                                   std::string_view    class_name,
                                   std::string_view    title )
    {
        const xcb_window_t                 window = xcb_generate_id( connection );
        const std::array<std::uint32_t, 1> values{ screen.black_pixel };

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
        set_title( connection, window, title );
        EXPECT_TRUE( request_succeeded( connection,
                                        xcb_map_window_checked( connection, window ) ) );
        EXPECT_TRUE( flush_succeeded( connection ) );
        return window;
    }

    [[nodiscard]]
    xcb_window_t
    create_test_window( xcb_connection_t*   connection,
                        const xcb_screen_t& screen )
    {
        return create_window_with_properties( connection,
                                              screen,
                                              windowInstance,
                                              windowClass,
                                              initialTitle );
    }

    [[nodiscard]]
    bool
    wait_for_reactor_barrier( grab::core::Reactor& reactor )
    {
        std::promise<void> registered;
        auto               registered_future = registered.get_future();
        reactor.post(
            [&registered]
            {
                registered.set_value();
            }
        );
        return registered_future.wait_for( registrationTimeout ) ==
               std::future_status::ready;
    }

    [[nodiscard]]
    std::optional<grab::Event>
    wait_for_event( grab::Subscription& subscription,
                    grab::EventKind     kind )
    {
        const auto deadline = std::chrono::steady_clock::now() + eventTimeout;
        while( std::chrono::steady_clock::now() < deadline )
        {
            while( auto event = subscription.try_pop() )
            {
                if( event->kind == kind )
                {
                    return event;
                }
            }
            std::this_thread::sleep_for( eventPollInterval );
        }
        return std::nullopt;
    }

    [[nodiscard]]
    grab::EventFilter
    create_destroy_filter()
    {
        grab::EventFilter filter;
        filter.kinds.push_back( grab::EventKind::WindowCreated );
        filter.kinds.push_back( grab::EventKind::WindowClosed );
        return filter;
    }

    [[nodiscard]]
    grab::EventFilter
    focus_title_filter()
    {
        grab::EventFilter filter;
        filter.kinds.push_back( grab::EventKind::WindowFocusChanged );
        filter.kinds.push_back( grab::EventKind::WindowTitleChanged );
        return filter;
    }

    [[nodiscard]]
    std::optional<grab::WindowChange>
    wait_for_window_change( grab::Subscription& subscription,
                            grab::EventKind     kind,
                            std::string_view    app,
                            std::string_view    title )
    {
        const auto deadline = std::chrono::steady_clock::now() + eventTimeout;
        while( std::chrono::steady_clock::now() < deadline )
        {
            while( auto event = subscription.try_pop() )
            {
                if( event->kind != kind )
                {
                    continue;
                }

                const auto* payload = std::get_if<grab::WindowChange>( &event->payload );
                if( payload !=
                    nullptr &&
                    payload->app ==
                    app &&
                    payload->title == title )
                {
                    return *payload;
                }
            }
            std::this_thread::sleep_for( eventPollInterval );
        }
        return std::nullopt;
    }

    void
    drain_events( grab::Subscription& subscription )
    {
        while( subscription.try_pop().has_value() )
        {
        }
    }

}    // namespace

TEST( WindowX11,
      CreateAndDestroyEmitEvents )
{
    RunningReactor running;
    ASSERT_TRUE( running.wait_until_started() ) << reactorDidNotStart;

    grab::EventBus bus;
    auto subscription = bus.subscribe( create_destroy_filter(), subscriptionDepth );

    auto tracker_result =
        grab::drivers::desktop::x11::WindowTracker::start( xvfbDisplay,
                                                           running.reactor(),
                                                           bus,
                                                           pollInterval );
    ASSERT_TRUE( tracker_result.has_value() ) << tracker_result.error().message;
    auto tracker = std::move( *tracker_result );
    ASSERT_TRUE( wait_for_reactor_barrier( running.reactor() ) ) << trackerNotReady;

    const TestConnection connection{ xvfbDisplay };
    ASSERT_NE( connection.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( connection.get() ), xcbOk );
    const xcb_screen_t* screen =
        default_screen( connection.get(), connection.screen_index() );
    ASSERT_NE( screen, nullptr );

    const xcb_window_t window = create_test_window( connection.get(), *screen );
    auto created = wait_for_event( subscription, grab::EventKind::WindowCreated );
    ASSERT_TRUE( created.has_value() );
    const auto* created_payload = std::get_if<grab::WindowChange>( &created->payload );
    ASSERT_NE( created_payload, nullptr );

    EXPECT_TRUE( request_succeeded( connection.get(),
                                    xcb_destroy_window_checked( connection.get(),
                                                                window ) ) );
    EXPECT_TRUE( flush_succeeded( connection.get() ) );

    auto closed = wait_for_event( subscription, grab::EventKind::WindowClosed );
    ASSERT_TRUE( closed.has_value() );
    const auto* closed_payload = std::get_if<grab::WindowChange>( &closed->payload );
    ASSERT_NE( closed_payload, nullptr );
    EXPECT_GE( closed_payload->duration_s, 0.0 );

    tracker.stop();
    running.stop_and_join();
    EXPECT_TRUE( running.result().has_value() );
}

TEST( WindowX11,
      ActiveWindowChangePublishesFocus )
{
    RunningReactor running;
    ASSERT_TRUE( running.wait_until_started() ) << reactorDidNotStart;

    grab::EventBus bus;
    auto           subscription = bus.subscribe(
        grab::EventFilter{
            .kinds      = { grab::EventKind::WindowFocusChanged },
            .categories = {},
        },
        subscriptionDepth
    );

    auto tracker_result =
        grab::drivers::desktop::x11::WindowTracker::start( xvfbDisplay,
                                                           running.reactor(),
                                                           bus,
                                                           pollInterval );
    ASSERT_TRUE( tracker_result.has_value() ) << trackerDidNotStart;
    auto tracker = std::move( *tracker_result );
    ASSERT_TRUE( wait_for_reactor_barrier( running.reactor() ) ) << trackerNotReady;

    const TestConnection connection{ xvfbDisplay };
    ASSERT_NE( connection.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( connection.get() ), xcbOk );
    const xcb_screen_t* screen =
        default_screen( connection.get(), connection.screen_index() );
    ASSERT_NE( screen, nullptr );

    const xcb_window_t window = create_test_window( connection.get(), *screen );
    drain_events( subscription );
    set_active_window( connection.get(), screen->root, window );
    EXPECT_TRUE( flush_succeeded( connection.get() ) );

    auto focused = wait_for_window_change( subscription,
                                           grab::EventKind::WindowFocusChanged,
                                           windowClass,
                                           initialTitle );
    ASSERT_TRUE( focused.has_value() );

    tracker.stop();
    running.stop_and_join();
    EXPECT_TRUE( running.result().has_value() );
}

TEST( WindowTracker,
      ActiveWindowChangeEmitsActiveChildDelta )
{
    RunningReactor running;
    ASSERT_TRUE( running.wait_until_started() ) << reactorDidNotStart;

    grab::EventBus bus;
    auto           subscription = bus.subscribe(
        grab::EventFilter{
            .kinds =
                {
                        grab::EventKind::WindowFocusChanged,
                        grab::EventKind::ActiveChildChanged,
                        },
            .categories = {  },
    },
        subscriptionDepth
    );

    const TestConnection connection{ xvfbDisplay };
    ASSERT_NE( connection.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( connection.get() ), xcbOk );
    const xcb_screen_t* screen =
        default_screen( connection.get(), connection.screen_index() );
    ASSERT_NE( screen, nullptr );
    set_active_window( connection.get(), screen->root, XCB_WINDOW_NONE );
    EXPECT_TRUE( flush_succeeded( connection.get() ) );

    auto tracker_result =
        grab::drivers::desktop::x11::WindowTracker::start( xvfbDisplay,
                                                           running.reactor(),
                                                           bus,
                                                           pollInterval );
    ASSERT_TRUE( tracker_result.has_value() ) << trackerDidNotStart;
    auto tracker = std::move( *tracker_result );
    ASSERT_TRUE( wait_for_reactor_barrier( running.reactor() ) ) << trackerNotReady;

    auto baseline = wait_for_event( subscription, grab::EventKind::WindowFocusChanged );
    ASSERT_TRUE( baseline.has_value() );

    const xcb_window_t first_window  = create_test_window( connection.get(), *screen );
    const xcb_window_t second_window = create_test_window( connection.get(), *screen );
    set_active_window( connection.get(), screen->root, first_window );
    EXPECT_TRUE( flush_succeeded( connection.get() ) );

    auto first_focus = wait_for_window_change( subscription,
                                               grab::EventKind::WindowFocusChanged,
                                               windowClass,
                                               initialTitle );
    ASSERT_TRUE( first_focus.has_value() );
    auto first_delta =
        wait_for_event( subscription, grab::EventKind::ActiveChildChanged );
    ASSERT_TRUE( first_delta.has_value() );

    set_active_window( connection.get(), screen->root, second_window );
    EXPECT_TRUE( flush_succeeded( connection.get() ) );

    auto changed = wait_for_event( subscription, grab::EventKind::ActiveChildChanged );
    ASSERT_TRUE( changed.has_value() );
    const auto* graph_change = std::get_if<grab::GraphChange>( &changed->payload );
    ASSERT_NE( graph_change, nullptr );
    EXPECT_EQ( graph_change->previous_active,
               static_cast<std::uint64_t>( first_window ) );
    EXPECT_EQ( graph_change->node, static_cast<std::uint64_t>( second_window ) );

    tracker.stop();
    running.stop_and_join();
    EXPECT_TRUE( running.result().has_value() );
}

TEST( WindowX11,
      TitleChangePublishesTitleChanged )
{
    RunningReactor running;
    ASSERT_TRUE( running.wait_until_started() ) << reactorDidNotStart;

    grab::EventBus bus;
    auto subscription = bus.subscribe( focus_title_filter(), subscriptionDepth );

    auto tracker_result =
        grab::drivers::desktop::x11::WindowTracker::start( xvfbDisplay,
                                                           running.reactor(),
                                                           bus,
                                                           pollInterval );
    ASSERT_TRUE( tracker_result.has_value() ) << trackerDidNotStart;
    auto tracker = std::move( *tracker_result );
    ASSERT_TRUE( wait_for_reactor_barrier( running.reactor() ) ) << trackerNotReady;

    const TestConnection connection{ xvfbDisplay };
    ASSERT_NE( connection.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( connection.get() ), xcbOk );
    const xcb_screen_t* screen =
        default_screen( connection.get(), connection.screen_index() );
    ASSERT_NE( screen, nullptr );

    const xcb_window_t window = create_test_window( connection.get(), *screen );
    set_active_window( connection.get(), screen->root, window );
    EXPECT_TRUE( flush_succeeded( connection.get() ) );

    auto focused = wait_for_window_change( subscription,
                                           grab::EventKind::WindowFocusChanged,
                                           windowClass,
                                           initialTitle );
    ASSERT_TRUE( focused.has_value() );
    set_title( connection.get(), window, changedTitle );
    EXPECT_TRUE( flush_succeeded( connection.get() ) );

    auto changed = wait_for_window_change( subscription,
                                           grab::EventKind::WindowTitleChanged,
                                           windowClass,
                                           changedTitle );
    ASSERT_TRUE( changed.has_value() );
    EXPECT_EQ( changed->prev_title, initialTitle );

    tracker.stop();
    running.stop_and_join();
    EXPECT_TRUE( running.result().has_value() );
}
