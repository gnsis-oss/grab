#include "grab/result.hpp"
#include "grab/space.hpp"
#include "grab/window_match.hpp"
#include "input/locator.hpp"
#include "platform/x11/protocol.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <xcb/xcb.h>
#include <xcb/xproto.h>
// clang-format on

namespace
{

    constexpr const char*      xvfbDisplay             = ":98";
    constexpr int              xcbOk                   = 0;

    constexpr std::int16_t     firstWindowX            = 140;
    constexpr std::int16_t     firstWindowY            = 180;
    constexpr std::int16_t     secondWindowX           = 360;
    constexpr std::int16_t     secondWindowY           = 240;
    constexpr std::uint16_t    windowWidth             = 220U;
    constexpr std::uint16_t    windowHeight            = 130U;
    constexpr std::uint16_t    windowBorderWidth       = 0U;
    constexpr std::uint32_t    windowValueMask         = XCB_CW_BACK_PIXEL;
    constexpr std::uint32_t    propertyReplaceMode     = XCB_PROP_MODE_REPLACE;
    constexpr std::uint8_t     format8Bits             = 8U;
    constexpr std::uint8_t     format32Bits            = 32U;
    constexpr std::uint8_t     responseTypeMask        = 0X7FU;
    constexpr auto             mapTimeout              = std::chrono::seconds{ 2 };
    constexpr auto             mapPollInterval         = std::chrono::milliseconds{ 10 };
    constexpr auto             activationResponseDelay = std::chrono::milliseconds{ 50 };
    constexpr auto             managerEventTimeout     = std::chrono::seconds{ 2 };

    constexpr std::string_view knownInstance           = "grab-locator-instance";
    constexpr std::string_view knownClass              = "GrabLocatorKnownClass";
    constexpr std::string_view sharedInstance          = "grab-locator-shared-instance";
    constexpr std::string_view sharedClass             = "GrabLocatorSharedClass";
    constexpr std::string_view nonTargetTitle          = "ordinary locator title";
    constexpr std::string_view targetTitle        = "prefix the-target-title suffix";
    constexpr std::string_view targetNeedle       = "the-target-title";
    constexpr std::string_view stackingClass      = "GrabLocatorStackingClass";
    constexpr std::string_view visibilityClass    = "GrabLocatorVisibilityClass";
    constexpr std::string_view missingClassName   = "class-that-does-not-exist";
    constexpr auto             windowNotFoundCode = grab::ErrorCode::WindowNotFound;

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

    class ScopedRootListProperties
    {
        public:

            ScopedRootListProperties( xcb_connection_t* connection,
                                      xcb_window_t      root,
                                      xcb_atom_t        client_list,
                                      xcb_atom_t        stacking_list ) noexcept :
                connection_( connection ),
                root_( root ),
                client_list_( client_list ),
                stacking_list_( stacking_list )
            {
            }

            ~ScopedRootListProperties()
            {
                if( connection_ == nullptr )
                {
                    return;
                }
                ( void )xcb_delete_property( connection_, root_, client_list_ );
                ( void )xcb_delete_property( connection_, root_, stacking_list_ );
                ( void )xcb_flush( connection_ );
            }

            ScopedRootListProperties( const ScopedRootListProperties& ) = delete;
            ScopedRootListProperties&
            operator=( const ScopedRootListProperties& )           = delete;
            ScopedRootListProperties( ScopedRootListProperties&& ) = delete;
            ScopedRootListProperties&
            operator=( ScopedRootListProperties&& ) = delete;

        private:

            xcb_connection_t* connection_    = nullptr;
            xcb_window_t      root_          = XCB_WINDOW_NONE;
            xcb_atom_t        client_list_   = XCB_ATOM_NONE;
            xcb_atom_t        stacking_list_ = XCB_ATOM_NONE;
    };

    [[nodiscard]]
    testing::AssertionResult
    set_window_list( xcb_connection_t*             connection,
                     xcb_window_t                  root,
                     xcb_atom_t                    property,
                     std::span<const xcb_window_t> windows )
    {
        return request_succeeded(
            connection,
            xcb_change_property_checked( connection,
                                         propertyReplaceMode,
                                         root,
                                         property,
                                         XCB_ATOM_WINDOW,
                                         format32Bits,
                                         static_cast<std::uint32_t>( windows.size() ),
                                         windows.data() )
        );
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

    [[nodiscard]]
    bool
    wait_for_map_notify( xcb_connection_t* connection,
                         xcb_window_t      window )
    {
        const auto deadline = std::chrono::steady_clock::now() + mapTimeout;
        while( std::chrono::steady_clock::now() < deadline )
        {
            const auto event = take_xcb_owned( xcb_poll_for_event( connection ) );
            if( event != nullptr )
            {
                const auto response_type =
                    static_cast<std::uint8_t>( event->response_type & responseTypeMask );
                if( response_type == XCB_MAP_NOTIFY )
                {
                    const void* const raw_event = event.get();
                    const auto* const map =
                        static_cast<const xcb_map_notify_event_t*>( raw_event );
                    if( map->window == window )
                    {
                        return true;
                    }
                }
                continue;
            }

            if( xcb_connection_has_error( connection ) != xcbOk )
            {
                return false;
            }
            std::this_thread::sleep_for( mapPollInterval );
        }
        return false;
    }

    [[nodiscard]]
    xcb_window_t
    create_test_window( xcb_connection_t*   connection,
                        const xcb_screen_t& screen,
                        std::int16_t        x,
                        std::int16_t        y,
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
                                                          x,
                                                          y,
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
    create_child_window( xcb_connection_t*   connection,
                         const xcb_screen_t& screen,
                         xcb_window_t        parent )
    {
        const xcb_window_t                 window = xcb_generate_id( connection );
        const std::array<std::uint32_t, 1> values{ screen.black_pixel };
        EXPECT_TRUE(
            request_succeeded( connection,
                               xcb_create_window_checked( connection,
                                                          screen.root_depth,
                                                          window,
                                                          parent,
                                                          0,
                                                          0,
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

    void
    apply_focus_after_activation_request( xcb_connection_t* connection,
                                          xcb_atom_t        active_window_atom,
                                          xcb_window_t      target,
                                          xcb_window_t      focus_child,
                                          std::atomic_bool& focus_applied )
    {
        const auto deadline = std::chrono::steady_clock::now() + managerEventTimeout;
        while( std::chrono::steady_clock::now() < deadline )
        {
            const auto event = take_xcb_owned( xcb_poll_for_event( connection ) );
            if( event == nullptr )
            {
                if( xcb_connection_has_error( connection ) != xcbOk )
                {
                    return;
                }
                std::this_thread::sleep_for( mapPollInterval );
                continue;
            }

            const auto response_type =
                static_cast<std::uint8_t>( event->response_type & responseTypeMask );
            if( response_type != XCB_CLIENT_MESSAGE )
            {
                continue;
            }

            const void* const raw_event = event.get();
            const auto* const message =
                static_cast<const xcb_client_message_event_t*>( raw_event );
            if( message->type != active_window_atom || message->window != target )
            {
                continue;
            }

            std::this_thread::sleep_for( activationResponseDelay );
            ( void )xcb_set_input_focus( connection,
                                         XCB_INPUT_FOCUS_POINTER_ROOT,
                                         focus_child,
                                         XCB_CURRENT_TIME );
            const bool flushed = xcb_flush( connection ) >
                                 0 &&
                                 xcb_connection_has_error( connection ) == xcbOk;
            focus_applied.store( flushed );
            return;
        }
    }

    [[nodiscard]]
    testing::AssertionResult
    setup_root_event_mask( xcb_connection_t*   connection,
                           const xcb_screen_t& screen )
    {
        constexpr std::uint32_t rootEventMask = XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
        constexpr std::uint32_t rootValueMask = XCB_CW_EVENT_MASK;
        return request_succeeded(
            connection,
            xcb_change_window_attributes_checked( connection,
                                                  screen.root,
                                                  rootValueMask,
                                                  &rootEventMask )
        );
    }

}    // namespace

TEST( Locator,
      LocatesWindowByWmClass )
{
    const TestConnection connection{ xvfbDisplay };
    ASSERT_NE( connection.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( connection.get() ), xcbOk );
    const xcb_screen_t* screen =
        default_screen( connection.get(), connection.screen_index() );
    ASSERT_NE( screen, nullptr );
    ASSERT_TRUE( setup_root_event_mask( connection.get(), *screen ) );

    const xcb_window_t window = create_test_window( connection.get(),
                                                    *screen,
                                                    firstWindowX,
                                                    firstWindowY,
                                                    knownInstance,
                                                    knownClass,
                                                    nonTargetTitle );
    ASSERT_TRUE( wait_for_map_notify( connection.get(), window ) );

    auto locator = grab::input::WindowLocator::open( xvfbDisplay );
    ASSERT_TRUE( locator.has_value() ) << locator.error().message;

    auto located = locator->locate( { std::string{ knownClass } } );

    ASSERT_TRUE( located.has_value() ) << located.error().message;
    EXPECT_EQ( located->window.xid, window );
    EXPECT_EQ( located->bounds.x, firstWindowX );
    EXPECT_EQ( located->bounds.y, firstWindowY );
    EXPECT_EQ( located->bounds.w, windowWidth );
    EXPECT_EQ( located->bounds.h, windowHeight );
    EXPECT_EQ( located->trust, grab::TransformTrust::Exact );
}

TEST( Locator,
      TitleFilterNarrowsMatch )
{
    const TestConnection connection{ xvfbDisplay };
    ASSERT_NE( connection.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( connection.get() ), xcbOk );
    const xcb_screen_t* screen =
        default_screen( connection.get(), connection.screen_index() );
    ASSERT_NE( screen, nullptr );
    ASSERT_TRUE( setup_root_event_mask( connection.get(), *screen ) );

    const xcb_window_t non_target = create_test_window( connection.get(),
                                                        *screen,
                                                        firstWindowX,
                                                        firstWindowY,
                                                        sharedInstance,
                                                        sharedClass,
                                                        nonTargetTitle );
    ASSERT_TRUE( wait_for_map_notify( connection.get(), non_target ) );
    const xcb_window_t target = create_test_window( connection.get(),
                                                    *screen,
                                                    secondWindowX,
                                                    secondWindowY,
                                                    sharedInstance,
                                                    sharedClass,
                                                    targetTitle );
    ASSERT_TRUE( wait_for_map_notify( connection.get(), target ) );

    auto locator = grab::input::WindowLocator::open( xvfbDisplay );
    ASSERT_TRUE( locator.has_value() ) << locator.error().message;

    auto located = locator->locate( { std::string{ sharedClass } }, targetNeedle );

    ASSERT_TRUE( located.has_value() ) << located.error().message;
    EXPECT_EQ( located->window.xid, target );
    EXPECT_EQ( located->bounds.x, secondWindowX );
    EXPECT_EQ( located->bounds.y, secondWindowY );
    EXPECT_EQ( located->bounds.w, windowWidth );
    EXPECT_EQ( located->bounds.h, windowHeight );
    EXPECT_EQ( located->trust, grab::TransformTrust::Exact );
}

TEST( Locator,
      PrefersTopmostStackingListWindow )
{
    const TestConnection connection{ xvfbDisplay };
    ASSERT_NE( connection.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( connection.get() ), xcbOk );
    const xcb_screen_t* screen =
        default_screen( connection.get(), connection.screen_index() );
    ASSERT_NE( screen, nullptr );
    ASSERT_TRUE( setup_root_event_mask( connection.get(), *screen ) );

    const xcb_window_t first = create_test_window( connection.get(),
                                                   *screen,
                                                   firstWindowX,
                                                   firstWindowY,
                                                   sharedInstance,
                                                   stackingClass,
                                                   nonTargetTitle );
    ASSERT_TRUE( wait_for_map_notify( connection.get(), first ) );
    const xcb_window_t topmost = create_test_window( connection.get(),
                                                     *screen,
                                                     secondWindowX,
                                                     secondWindowY,
                                                     sharedInstance,
                                                     stackingClass,
                                                     targetTitle );
    ASSERT_TRUE( wait_for_map_notify( connection.get(), topmost ) );

    xcb_atom_t client_list   = XCB_ATOM_NONE;
    xcb_atom_t stacking_list = XCB_ATOM_NONE;
    ASSERT_TRUE( intern_atom( connection.get(),
                              grab::platform::x11::atom_name::netClientList,
                              client_list ) );
    ASSERT_TRUE( intern_atom( connection.get(),
                              grab::platform::x11::atom_name::netClientListStacking,
                              stacking_list ) );
    const ScopedRootListProperties properties{
        connection.get(),
        screen->root,
        client_list,
        stacking_list,
    };

    const std::array<xcb_window_t, 2> ordinary_order{ topmost, first };
    const std::array<xcb_window_t, 2> stacking_order{ first, topmost };
    ASSERT_TRUE(
        set_window_list( connection.get(), screen->root, client_list, ordinary_order )
    );
    ASSERT_TRUE(
        set_window_list( connection.get(), screen->root, stacking_list, stacking_order )
    );

    auto locator = grab::input::WindowLocator::open( xvfbDisplay );
    ASSERT_TRUE( locator.has_value() ) << locator.error().message;

    auto located = locator->locate( { std::string{ stackingClass } } );

    ASSERT_TRUE( located.has_value() ) << located.error().message;
    EXPECT_EQ( located->window.xid, topmost );
}

TEST( Locator,
      SkipsUnmappedStackingListWindow )
{
    const TestConnection connection{ xvfbDisplay };
    ASSERT_NE( connection.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( connection.get() ), xcbOk );
    const xcb_screen_t* screen =
        default_screen( connection.get(), connection.screen_index() );
    ASSERT_NE( screen, nullptr );
    ASSERT_TRUE( setup_root_event_mask( connection.get(), *screen ) );

    const xcb_window_t visible = create_test_window( connection.get(),
                                                     *screen,
                                                     firstWindowX,
                                                     firstWindowY,
                                                     sharedInstance,
                                                     visibilityClass,
                                                     nonTargetTitle );
    ASSERT_TRUE( wait_for_map_notify( connection.get(), visible ) );
    const xcb_window_t hidden = create_test_window( connection.get(),
                                                    *screen,
                                                    secondWindowX,
                                                    secondWindowY,
                                                    sharedInstance,
                                                    visibilityClass,
                                                    targetTitle );
    ASSERT_TRUE( wait_for_map_notify( connection.get(), hidden ) );
    ASSERT_TRUE( request_succeeded( connection.get(),
                                    xcb_unmap_window_checked( connection.get(),
                                                              hidden ) ) );

    xcb_atom_t client_list   = XCB_ATOM_NONE;
    xcb_atom_t stacking_list = XCB_ATOM_NONE;
    ASSERT_TRUE( intern_atom( connection.get(),
                              grab::platform::x11::atom_name::netClientList,
                              client_list ) );
    ASSERT_TRUE( intern_atom( connection.get(),
                              grab::platform::x11::atom_name::netClientListStacking,
                              stacking_list ) );
    const ScopedRootListProperties properties{
        connection.get(),
        screen->root,
        client_list,
        stacking_list,
    };
    const std::array<xcb_window_t, 2> stacking_order{ visible, hidden };
    ASSERT_TRUE(
        set_window_list( connection.get(), screen->root, client_list, stacking_order )
    );
    ASSERT_TRUE(
        set_window_list( connection.get(), screen->root, stacking_list, stacking_order )
    );

    auto locator = grab::input::WindowLocator::open( xvfbDisplay );
    ASSERT_TRUE( locator.has_value() ) << locator.error().message;

    auto located = locator->locate( { std::string{ visibilityClass } } );

    ASSERT_TRUE( located.has_value() ) << located.error().message;
    EXPECT_EQ( located->window.xid, visible );
}

TEST( Locator,
      ActivateWaitsForFocusedDescendant )
{
    const TestConnection connection{ xvfbDisplay };
    ASSERT_NE( connection.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( connection.get() ), xcbOk );
    const xcb_screen_t* screen =
        default_screen( connection.get(), connection.screen_index() );
    ASSERT_NE( screen, nullptr );
    ASSERT_TRUE( setup_root_event_mask( connection.get(), *screen ) );

    const xcb_window_t target = create_test_window( connection.get(),
                                                    *screen,
                                                    firstWindowX,
                                                    firstWindowY,
                                                    knownInstance,
                                                    knownClass,
                                                    targetTitle );
    ASSERT_TRUE( wait_for_map_notify( connection.get(), target ) );
    const xcb_window_t focus_child =
        create_child_window( connection.get(), *screen, target );
    ASSERT_TRUE(
        request_succeeded( connection.get(),
                           xcb_set_input_focus_checked( connection.get(),
                                                        XCB_INPUT_FOCUS_POINTER_ROOT,
                                                        screen->root,
                                                        XCB_CURRENT_TIME ) )
    );

    xcb_atom_t active_window = XCB_ATOM_NONE;
    ASSERT_TRUE( intern_atom( connection.get(),
                              grab::platform::x11::atom_name::netActiveWindow,
                              active_window ) );
    auto locator = grab::input::WindowLocator::open( xvfbDisplay );
    ASSERT_TRUE( locator.has_value() ) << locator.error().message;

    std::atomic_bool focus_applied = false;
    std::thread      manager{
        apply_focus_after_activation_request,
        connection.get(),
        active_window,
        target,
        focus_child,
        std::ref( focus_applied )
    };

    grab::input::LocatedWindow target_window{};
    target_window.window.xid                   = target;
    const auto activation                      = locator->activate( target_window );
    const bool focus_was_applied_before_return = focus_applied.load();
    manager.join();

    ASSERT_TRUE( activation.has_value() ) << activation.error().message;
    EXPECT_TRUE( focus_was_applied_before_return );
}

TEST( Locator,
      NotFoundReturnsError )
{
    auto locator = grab::input::WindowLocator::open( xvfbDisplay );
    ASSERT_TRUE( locator.has_value() ) << locator.error().message;

    auto located = locator->locate( { std::string{ missingClassName } } );

    ASSERT_FALSE( located.has_value() );
    EXPECT_EQ( located.error().code, windowNotFoundCode );
}
