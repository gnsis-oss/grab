#include "client/client.hpp"
#include "client/loopback_transport.hpp"
#include "compat/eventgrab_v1/browser_projection.hpp"
#include "core/reactor.hpp"
#include "drivers/desktop/x11/enumerate.hpp"
#include "grab/capture.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/ids.hpp"
#include "grab/interaction.hpp"
#include "grab/locator.hpp"
#include "grab/origin.hpp"
#include "grab/session.hpp"
#include "grab/space.hpp"
#include "kernel/lifecycle/session_impl.hpp"
#include "transport/codec.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
// clang-format on

namespace
{

    constexpr std::string_view windowTitle{ "grab Phase-1 EXIT-GATE" };
    constexpr std::string_view changedWindowTitle{ "grab Phase-1 EXIT-GATE changed" };
    constexpr std::string_view windowClass{ "grab-exit-gate" };
    constexpr std::string_view netClientListAtom{ "_NET_CLIENT_LIST" };
    constexpr std::uint16_t    windowX      = 31U;
    constexpr std::uint16_t    windowY      = 37U;
    constexpr std::uint16_t    windowWidth  = 320U;
    constexpr std::uint16_t    windowHeight = 180U;
    constexpr std::uint8_t     format32Bits = 32U;
    constexpr std::int64_t     browserPid   = 4'242;

    struct XcbWindowGuard
    {
            xcb_connection_t* connection{};
            xcb_window_t      window{ XCB_WINDOW_NONE };

            ~XcbWindowGuard()
            {
                if( connection == nullptr )
                {
                    return;
                }
                if( window != XCB_WINDOW_NONE )
                {
                    static_cast<void>( xcb_destroy_window( connection, window ) );
                    static_cast<void>( xcb_flush( connection ) );
                }
                xcb_disconnect( connection );
            }

            XcbWindowGuard()                        = default;
            XcbWindowGuard( const XcbWindowGuard& ) = delete;
            XcbWindowGuard&
            operator=( const XcbWindowGuard& ) = delete;
    };

    [[nodiscard]]
    bool
    request_succeeded( xcb_connection_t* connection,
                       xcb_void_cookie_t request )
    {
        auto* const error = xcb_request_check( connection, request );
        if( error == nullptr )
        {
            return true;
        }
        std::free( error );
        return false;
    }

    [[nodiscard]]
    xcb_atom_t
    intern_atom( xcb_connection_t* connection,
                 std::string_view  name )
    {
        const auto  cookie = xcb_intern_atom( connection,
                                              0U,
                                              static_cast<std::uint16_t>( name.size() ),
                                              name.data() );
        auto* const reply  = xcb_intern_atom_reply( connection, cookie, nullptr );
        if( reply == nullptr )
        {
            return XCB_ATOM_NONE;
        }
        const auto atom = reply->atom;
        std::free( reply );
        return atom;
    }

    [[nodiscard]]
    bool
    set_text_property( xcb_connection_t* connection,
                       xcb_window_t      window,
                       xcb_atom_t        property,
                       xcb_atom_t        type,
                       std::string_view  value )
    {
        return request_succeeded(
            connection,
            xcb_change_property_checked( connection,
                                         XCB_PROP_MODE_REPLACE,
                                         window,
                                         property,
                                         type,
                                         8U,
                                         static_cast<std::uint32_t>( value.size() ),
                                         value.data() )
        );
    }

    [[nodiscard]]
    bool
    set_client_list( xcb_connection_t* connection,
                     xcb_window_t      root,
                     xcb_window_t      window )
    {
        const auto net_client_list = intern_atom( connection, netClientListAtom );
        if( net_client_list == XCB_ATOM_NONE )
        {
            return false;
        }

        return request_succeeded( connection,
                                  xcb_change_property_checked( connection,
                                                               XCB_PROP_MODE_REPLACE,
                                                               root,
                                                               net_client_list,
                                                               XCB_ATOM_WINDOW,
                                                               format32Bits,
                                                               1U,
                                                               &window ) );
    }

}    // namespace

TEST( ExitGate,
      Phase1VerticalSliceOnX11 )
{
    const char* const display = std::getenv( "DISPLAY" );
    if( display == nullptr || std::string_view{ display }.empty() )
    {
        GTEST_SKIP() << "requires Xvfb (DISPLAY is not set)";
    }

    int            screen_number = 0;
    XcbWindowGuard test_window;
    test_window.connection = xcb_connect( display, &screen_number );
    if( test_window.connection ==
        nullptr ||
        xcb_connection_has_error( test_window.connection ) != 0 )
    {
        GTEST_SKIP() << "requires Xvfb (cannot connect to DISPLAY=" << display << ')';
    }

    auto screen_iterator =
        xcb_setup_roots_iterator( xcb_get_setup( test_window.connection ) );
    for( int index = 0; index < screen_number && screen_iterator.rem != 0; ++index )
    {
        xcb_screen_next( &screen_iterator );
    }
    ASSERT_NE( screen_iterator.rem, 0 );
    ASSERT_NE( screen_iterator.data, nullptr );
    const auto* const screen           = screen_iterator.data;

    test_window.window                 = xcb_generate_id( test_window.connection );
    constexpr std::uint32_t event_mask = XCB_EVENT_MASK_EXPOSURE |
                                         XCB_EVENT_MASK_STRUCTURE_NOTIFY |
                                         XCB_EVENT_MASK_PROPERTY_CHANGE;
    const std::uint32_t     values[]{ screen->black_pixel, event_mask };
    ASSERT_TRUE( request_succeeded(
        test_window.connection,
        xcb_create_window_checked( test_window.connection,
                                   XCB_COPY_FROM_PARENT,
                                   test_window.window,
                                   screen->root,
                                   windowX,
                                   windowY,
                                   windowWidth,
                                   windowHeight,
                                   0U,
                                   XCB_WINDOW_CLASS_INPUT_OUTPUT,
                                   screen->root_visual,
                                   XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK,
                                   values )
    ) );

    ASSERT_TRUE( set_text_property( test_window.connection,
                                    test_window.window,
                                    XCB_ATOM_WM_NAME,
                                    XCB_ATOM_STRING,
                                    windowTitle ) );

    std::string wm_class;
    wm_class.reserve( ( windowClass.size() * 2U ) + 2U );
    wm_class.append( windowClass );
    wm_class.push_back( '\0' );
    wm_class.append( windowClass );
    wm_class.push_back( '\0' );
    ASSERT_TRUE( set_text_property( test_window.connection,
                                    test_window.window,
                                    XCB_ATOM_WM_CLASS,
                                    XCB_ATOM_STRING,
                                    wm_class ) );

    const auto utf8_string = intern_atom( test_window.connection, "UTF8_STRING" );
    const auto net_wm_name = intern_atom( test_window.connection, "_NET_WM_NAME" );
    ASSERT_NE( utf8_string, XCB_ATOM_NONE );
    ASSERT_NE( net_wm_name, XCB_ATOM_NONE );
    ASSERT_TRUE( set_text_property( test_window.connection,
                                    test_window.window,
                                    net_wm_name,
                                    utf8_string,
                                    windowTitle ) );
    ASSERT_TRUE( request_succeeded( test_window.connection,
                                    xcb_map_window_checked( test_window.connection,
                                                            test_window.window ) ) );
    ASSERT_TRUE(
        set_client_list( test_window.connection, screen->root, test_window.window )
    );
    ASSERT_GT( xcb_flush( test_window.connection ), 0 );

    auto first_session  = grab::Session::open( grab::SessionOptions{
        .display = std::string{ display },
        .seat    = {},
    } );
    auto second_session = grab::Session::open( grab::SessionOptions{
        .display = std::string{ display },
        .seat    = {},
    } );
    ASSERT_TRUE( first_session.has_value() );
    ASSERT_TRUE( second_session.has_value() );
    ASSERT_NE( first_session->get(), nullptr );
    ASSERT_NE( second_session->get(), nullptr );
    EXPECT_NE( first_session->get(), second_session->get() );
    EXPECT_TRUE( ( *first_session )->is_open() );
    EXPECT_TRUE( ( *second_session )->is_open() );

    // Session isolation: each composition root owns a private bus; events
    // published in one session must never surface in another.
    auto first_core  = grab::kernel::lifecycle::SessionCore::open( grab::SessionOptions{
        .display = std::string{ display },
        .seat    = {},
    } );
    auto second_core = grab::kernel::lifecycle::SessionCore::open( grab::SessionOptions{
        .display = std::string{ display },
        .seat    = {},
    } );
    ASSERT_TRUE( first_core.has_value() ) << first_core.error().message;
    ASSERT_TRUE( second_core.has_value() ) << second_core.error().message;

    // NOLINTBEGIN(readability-trailing-comma)
    auto        first_watch  = ( *first_core )
                                   ->bus()
                                   .subscribe( grab::SubscriptionScope{
                                       .kinds  = { grab::EventKind::NodeChanged },
                                       .filter = {},
                                   } );
    auto        second_watch = ( *second_core )
                                   ->bus()
                                   .subscribe( grab::SubscriptionScope{
                                       .kinds  = { grab::EventKind::NodeChanged },
                                       .filter = {},
                                   } );
    // NOLINTEND(readability-trailing-comma)

    grab::Event synthetic;
    synthetic.kind     = grab::EventKind::NodeChanged;
    synthetic.category = grab::EventCategory::Window;
    ( *first_core )->bus().publish( synthetic );

    const auto delivered = first_watch.try_pop();
    ASSERT_TRUE( delivered.has_value() );
    EXPECT_EQ( delivered->kind, grab::EventKind::NodeChanged );
    EXPECT_FALSE( second_watch.try_pop().has_value() );

    const auto locator = grab::sel::all(
        { grab::sel::role( grab::role::window ),
          grab::sel::property( grab::property::title, std::string{ windowTitle } ),
          grab::sel::property( grab::property::window_class,
                               std::string{ windowClass } ) }
    );
    const auto serialized_locator = locator.to_string();
    ASSERT_FALSE( serialized_locator.empty() );
    const auto reparsed_locator = grab::Locator::from_string( serialized_locator );
    ASSERT_TRUE( reparsed_locator.has_value() );
    EXPECT_EQ( *reparsed_locator, locator );

    const auto session_match = ( *first_session )->resolve( locator );
    ASSERT_TRUE( session_match.has_value() ) << session_match.error().message;
    EXPECT_NE( session_match->ref.node, 0U );
    EXPECT_NE( session_match->ref.generation.value, 0U );
    EXPECT_NE( session_match->provenance.runtime.value, 0U );
    EXPECT_GT( session_match->snapshot_revision, 0U );

    // NOLINTBEGIN(readability-trailing-comma)
    auto input_watch =
        ( *first_session )
            ->watch( grab::SubscriptionScope{
                .kinds  = { grab::EventKind::MouseClick, grab::EventKind::KeyDown },
                .filter = {},
    } );
    // NOLINTEND(readability-trailing-comma)
    ASSERT_TRUE( input_watch.has_value() ) << input_watch.error().message;
    EXPECT_NE( input_watch->id(), grab::SubscriptionId{} );

    const auto click_receipt =
        ( *first_session )->perform( grab::Click{ .target = *session_match } );
    ASSERT_TRUE( click_receipt.has_value() ) << click_receipt.error().message;
    EXPECT_TRUE( click_receipt->commit ==
                 grab::CommitStatus::Committed ||
                 click_receipt->commit == grab::CommitStatus::Verified );
    EXPECT_FALSE( click_receipt->routes.empty() );

    const auto type_receipt =
        ( *first_session )
            ->perform( grab::TypeText{ .target = *session_match, .text = "phase one" } );
    ASSERT_TRUE( type_receipt.has_value() ) << type_receipt.error().message;
    EXPECT_TRUE( type_receipt->commit ==
                 grab::CommitStatus::Committed ||
                 type_receipt->commit == grab::CommitStatus::Verified );
    EXPECT_FALSE( type_receipt->routes.empty() );

    std::size_t injected_events = 0U;
    while( auto event = input_watch->try_pop() )
    {
        if( event->kind !=
            grab::EventKind::MouseClick &&
            event->kind != grab::EventKind::KeyDown )
        {
            continue;
        }
        EXPECT_EQ( event->origin, grab::EventOrigin::InjectedSelf );
        ++injected_events;
    }
    EXPECT_GT( injected_events, 0U );

    // Output-name capture through the public Session verb.
    const auto outputs = grab::screen::list_outputs();
    ASSERT_TRUE( outputs.has_value() );
    ASSERT_FALSE( outputs->empty() );
    auto output_frame =
        ( *first_session )->capture( grab::CaptureTarget{ outputs->front().name } );
    ASSERT_TRUE( output_frame.has_value() ) << output_frame.error().message;
    EXPECT_NE( output_frame->id.value, 0U );
    EXPECT_NE( output_frame->space, grab::CoordinateSpaceId{} );
    EXPECT_NE( output_frame->generation, grab::CaptureGeneration{} );

    // Match-target capture: resolve the test window through the session's own
    // store, then capture that window's surface.
    auto window_frame =
        ( *first_session )->capture( grab::CaptureTarget{ *session_match } );
    ASSERT_TRUE( window_frame.has_value() ) << window_frame.error().message;
    EXPECT_NE( window_frame->id.value, 0U );
    EXPECT_EQ( window_frame->image.width, windowWidth );
    EXPECT_EQ( window_frame->image.height, windowHeight );

    ASSERT_TRUE( set_text_property( test_window.connection,
                                    test_window.window,
                                    XCB_ATOM_WM_NAME,
                                    XCB_ATOM_STRING,
                                    changedWindowTitle ) );
    ASSERT_TRUE( set_text_property( test_window.connection,
                                    test_window.window,
                                    net_wm_name,
                                    utf8_string,
                                    changedWindowTitle ) );
    ASSERT_GT( xcb_flush( test_window.connection ), 0 );

    const auto changed_locator = grab::sel::all(
        { grab::sel::role( grab::role::window ),
          grab::sel::property( grab::property::title,
                               std::string{ changedWindowTitle } ),
          grab::sel::property( grab::property::window_class,
                               std::string{ windowClass } ) }
    );

    auto client_session = grab::Session::open( grab::SessionOptions{
        .display = std::string{ display },
        .seat    = {},
    } );
    ASSERT_TRUE( client_session.has_value() ) << client_session.error().message;
    grab::client::LoopbackTransport loopback{ std::move( *client_session ) };
    grab::client::Client            client{ loopback };

    const auto                      client_match = client.resolve( changed_locator );
    ASSERT_TRUE( client_match.has_value() ) << client_match.error().message;

    auto client_output_frame =
        client.capture( grab::CaptureTarget{ outputs->front().name },
                        grab::CaptureOptions{} );
    ASSERT_TRUE( client_output_frame.has_value() )
        << client_output_frame.error().message;
    EXPECT_NE( client_output_frame->id.value, 0U );

    auto client_window_frame =
        client.capture( grab::CaptureTarget{ *client_match }, grab::CaptureOptions{} );
    ASSERT_TRUE( client_window_frame.has_value() )
        << client_window_frame.error().message;
    EXPECT_EQ( client_window_frame->image.width, windowWidth );
    EXPECT_EQ( client_window_frame->image.height, windowHeight );

    const auto client_receipt =
        client.perform( grab::Click{ .target = *client_match }, grab::ActionOptions{} );
    ASSERT_TRUE( client_receipt.has_value() ) << client_receipt.error().message;
    EXPECT_TRUE( client_receipt->commit ==
                 grab::CommitStatus::Committed ||
                 client_receipt->commit == grab::CommitStatus::Verified );

    const grab::compat::eventgrab_v1::BrowserTabProjection projection{
        .app            = "Firefox",
        .pid            = grab::Pid{ browserPid },
        .tab_title      = "Exit gate",
        .prev_tab_title = "Phase zero",
    };
    const auto wire_event = grab::compat::eventgrab_v1::to_wire( projection );
    EXPECT_EQ( wire_event.kind(), eventgrab::v1::BROWSER_TAB_SWITCHED );
    EXPECT_EQ( wire_event.category(), eventgrab::v1::EVENT_CATEGORY_BROWSER );
    std::string wire_bytes;
    ASSERT_TRUE( wire_event.SerializeToString( &wire_bytes ) );
    eventgrab::v1::Event parsed_wire;
    ASSERT_TRUE( parsed_wire.ParseFromString( wire_bytes ) );
    const auto decoded = grab::compat::eventgrab_v1::from_wire( parsed_wire );
    ASSERT_TRUE( decoded.has_value() );
    EXPECT_EQ( decoded->app, "Firefox" );
    EXPECT_EQ( decoded->pid, grab::Pid{ browserPid } );
    EXPECT_EQ( decoded->tab_title, "Exit gate" );
    EXPECT_EQ( decoded->prev_tab_title, "Phase zero" );
}

TEST( ExitGate,
      SessionCoreOpensAndAtspiAttachesOrRecordsUnavailability )
{
    const char* const display = std::getenv( "DISPLAY" );
    if( display == nullptr || std::string_view{ display }.empty() )
    {
        GTEST_SKIP() << "requires Xvfb (DISPLAY is not set)";
    }

    int            screen_number = 0;
    XcbWindowGuard display_probe;
    display_probe.connection = xcb_connect( display, &screen_number );
    if( display_probe.connection ==
        nullptr ||
        xcb_connection_has_error( display_probe.connection ) != 0 )
    {
        GTEST_SKIP() << "requires Xvfb (cannot connect to DISPLAY=" << display << ')';
    }

    grab::core::Reactor reactor;
    auto                core = grab::kernel::lifecycle::SessionCore::open(
        grab::SessionOptions{ .display = std::string{ display }, .seat = {} },
        &reactor
    );
    ASSERT_TRUE( core.has_value() ) << core.error().message;
    EXPECT_TRUE( ( *core )->store().snapshot().has_value() );

    constexpr std::size_t primaryStoreCount   = 1U;
    constexpr std::size_t attachedStoreCount  = 2U;
    constexpr std::size_t secondaryStoreIndex = 1U;

    const auto&           diagnostics         = ( *core )->runtime_diagnostics();
    const bool            hasAtspiDiagnostic =
        std::ranges::any_of( diagnostics,
                             []( const auto& diagnostic )
                             {
                                 return diagnostic.message.find( "atspi" ) !=
                                        std::string::npos;
                             } );

    EXPECT_TRUE( std::ranges::none_of( diagnostics,
                                       []( const auto& diagnostic )
                                       {
                                           return diagnostic.message.find(
                                                      "deferred"
                                                  ) != std::string::npos;
                                       } ) );

    if( !hasAtspiDiagnostic )
    {
        EXPECT_EQ( ( *core )->store_count(), attachedStoreCount );
        auto* const atspi_store = ( *core )->store_at( secondaryStoreIndex );
        ASSERT_NE( atspi_store, nullptr );

        const auto atspi_snapshot = atspi_store->snapshot();
        ASSERT_TRUE( atspi_snapshot.has_value() );
        const auto primary_snapshot = ( *core )->store().snapshot();
        ASSERT_TRUE( primary_snapshot.has_value() );
        EXPECT_NE( atspi_snapshot->runtime, primary_snapshot->runtime );
        return;
    }

    EXPECT_TRUE( std::ranges::none_of( diagnostics,
                                       []( const auto& diagnostic )
                                       {
                                           const auto& message = diagnostic.message;
                                           return message.find( "atspi" ) !=
                                                  std::string::npos &&
                                                  message.find( "unavailable" ) ==
                                                  std::string::npos &&
                                                  message.find( "skipped" ) ==
                                                  std::string::npos;
                                       } ) );
    EXPECT_EQ( ( *core )->store_count(), primaryStoreCount );
}
