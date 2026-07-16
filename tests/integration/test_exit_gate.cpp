#include "core/reactor.hpp"
#include "drivers/desktop/x11/x11_runtime.hpp"
#include "drivers/desktop/x11/x11_tree_source.hpp"
#include "grab/capture.hpp"
#include "grab/context.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/ids.hpp"
#include "grab/interaction.hpp"
#include "grab/locator.hpp"
#include "grab/session.hpp"
#include "grab/space.hpp"
#include "kernel/lifecycle/session_impl.hpp"
#include "screen/enumerate.hpp"
#include "transport/codec.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
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
    constexpr std::uint32_t    firstTree    = 1U;
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

    [[nodiscard]]
    bool
    property_equals( const grab::UiNodeRecord& node,
                     grab::PropertyId          property,
                     std::string_view          expected )
    {
        const auto  value = node.property( property );
        const auto* text  = std::get_if<std::string>( &value.value );
        return value.state ==
               grab::PropertyRead::State::Present &&
               text !=
               nullptr &&
             *text == expected;
    }

    [[nodiscard]]
    std::vector<const grab::UiNodeRecord*>
    matching_windows( const grab::UiSnapshot& snapshot,
                      std::string_view        title )
    {
        std::vector<const grab::UiNodeRecord*> matches;
        for( const auto& node : snapshot.nodes() )
        {
            if( node.role ==
                grab::role::window &&
                property_equals( node, grab::property::title, title ) &&
                property_equals( node, grab::property::window_class, windowClass ) )
            {
                matches.push_back( &node );
            }
        }
        return matches;
    }

    [[nodiscard]]
    bool
    has_route( const grab::drivers::desktop::x11::X11Runtime& runtime,
               std::string_view                               name )
    {
        return std::ranges::any_of( runtime.routes(),
                                    [name]( const auto& route )
                                    {
                                        return route.name == name;
                                    } );
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

    grab::drivers::desktop::x11::X11Runtime runtime;
    const grab::OperationContext            context{
        .deadline = grab::Deadline::unbounded(),
    };
    ASSERT_TRUE( runtime.start( context ).has_value() );
    ASSERT_NE( runtime.tree_source(), nullptr );

    // TODO(phase1): Session has no attach/register-runtime operation yet.  Starting
    // the runtime beside two live Sessions is the closest currently exposed seam.
    EXPECT_EQ( runtime.name(), std::string_view{ "x11" } );
    EXPECT_NE( runtime.target_registry(), nullptr );

    auto initial_snapshot = runtime.tree_source()->snapshot( firstTree, context );
    ASSERT_TRUE( initial_snapshot.has_value() );

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

    // TODO(phase1): Locator has no resolve()/exactly_one() API.  Apply the same
    // predicates to the generic snapshot and prove the single node maps back to
    // the native test XID; replace this with locator.exactly_one() once exposed.
    const auto matches = matching_windows( *initial_snapshot, windowTitle );
    ASSERT_EQ( matches.size(), 1U );
    const auto* const node = matches.front();
    ASSERT_NE( node, nullptr );

    const grab::WidgetRef resolved_ref{
        .runtime    = initial_snapshot->runtime,
        .tree       = initial_snapshot->tree,
        .epoch      = initial_snapshot->epoch,
        .node       = node->id.value,
        .generation = node->generation,
    };
    auto* const x11_source = dynamic_cast<grab::drivers::desktop::x11::X11TreeSource*>(
        runtime.tree_source()
    );
    ASSERT_NE( x11_source, nullptr );
    const auto resolved_xid = x11_source->resolve_xid( resolved_ref );
    ASSERT_TRUE( resolved_xid.has_value() );
    EXPECT_EQ( *resolved_xid, test_window.window );
    EXPECT_NE( node->generation.value, 0U );
    EXPECT_NE( node->provenance().runtime.value, 0U );
    EXPECT_NE( node->provenance().revision, 0U );

    const auto bounds_read = node->property( grab::property::bounds );
    ASSERT_EQ( bounds_read.state, grab::PropertyRead::State::Present );
    const auto* const bounds = std::get_if<grab::SpaceRect>( &bounds_read.value );
    ASSERT_NE( bounds, nullptr );
    EXPECT_NE( bounds->space.value, 0U );
    EXPECT_GT( bounds->w, 0.0 );
    EXPECT_GT( bounds->h, 0.0 );

    EXPECT_TRUE( has_route( runtime, "x11.pointer" ) );
    EXPECT_TRUE( has_route( runtime, "x11.keyboard" ) );
    EXPECT_TRUE( has_route( runtime, "x11.capture" ) );

    const grab::Match resolved_match{
        .ref                = resolved_ref,
        .mode               = grab::ConsistencyMode::Live,
        .snapshot_revision  = initial_snapshot->revision,
        .matched_predicates = {"role=window","title","window_class"                                },
        .provenance         = grab::ProviderProvenance{
                               .provider           = "x11",
                               .candidate_provider = "x11",
                               .runtime            = initial_snapshot->runtime,.revision           = initial_snapshot->revision,
                               },
    };
    const grab::Click    click{ .target = resolved_match };
    const grab::TypeText type_text{
        .target = resolved_match,
        .text   = "phase one",
    };
    ASSERT_NE( std::get_if<grab::Match>( &click.target ), nullptr );
    ASSERT_NE( std::get_if<grab::Match>( &type_text.target ), nullptr );
    EXPECT_EQ( std::get<grab::Match>( click.target ).ref, resolved_ref );
    EXPECT_EQ( std::get<grab::Match>( type_text.target ).ref, resolved_ref );

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
    const auto session_match = ( *first_session )->resolve( locator );
    ASSERT_TRUE( session_match.has_value() ) << session_match.error().message;
    auto window_frame =
        ( *first_session )->capture( grab::CaptureTarget{ *session_match } );
    ASSERT_TRUE( window_frame.has_value() ) << window_frame.error().message;
    EXPECT_NE( window_frame->id.value, 0U );
    EXPECT_EQ( window_frame->image.width, windowWidth );
    EXPECT_EQ( window_frame->image.height, windowHeight );

    // TODO(phase1): the exit gate still exercises no public perform(); the
    // Task-9 rewrite drives every verb through Session/client.

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

    auto changed_snapshot = x11_source->snapshot( firstTree, context );
    ASSERT_TRUE( changed_snapshot.has_value() );
    EXPECT_GT( changed_snapshot->revision, initial_snapshot->revision );
    EXPECT_EQ( matching_windows( *changed_snapshot, changedWindowTitle ).size(), 1U );

    // TODO(phase1): The tree source exposes initial/current state, but Session has
    // no node watch API that yields that initial state plus a SubscriptionId.

    const grab::Event legacy_event{
        .timestamp = 1.25,
        .sequence  = 7U,
        .kind      = grab::EventKind::BrowserTabSwitched,
        .category  = grab::EventCategory::Browser,
        .payload   = grab::BrowserTab{
                                      .app            = "Firefox",
                                      .pid            = grab::Pid{ browserPid },
                                      .tab_title      = "Exit gate",
                                      .prev_tab_title = "Phase zero",
                                      },
    };
    const auto wire_event = grab::transport::to_wire( legacy_event );
    ASSERT_TRUE( wire_event.has_value() );
    EXPECT_EQ( wire_event->kind(), eventgrab::v1::BROWSER_TAB_SWITCHED );

    const auto decoded_event = grab::transport::from_wire( *wire_event );
    ASSERT_TRUE( decoded_event.has_value() );
    EXPECT_EQ( decoded_event->kind, grab::EventKind::BrowserTabSwitched );
    EXPECT_EQ( decoded_event->category, grab::EventCategory::Browser );
    const auto* const decoded_tab =
        std::get_if<grab::BrowserTab>( &decoded_event->payload );
    ASSERT_NE( decoded_tab, nullptr );
    EXPECT_EQ( decoded_tab->app, "Firefox" );
    EXPECT_EQ( decoded_tab->pid, grab::Pid{ browserPid } );
    EXPECT_EQ( decoded_tab->tab_title, "Exit gate" );
    EXPECT_EQ( decoded_tab->prev_tab_title, "Phase zero" );

    EXPECT_TRUE( runtime.stop().has_value() );
}

TEST( ExitGate,
      SessionCoreOpensWhenAtspiUnavailableAndRecordsDiagnostic )
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

    const auto& diagnostics = ( *core )->runtime_diagnostics();
    ASSERT_FALSE( diagnostics.empty() );
    EXPECT_TRUE( std::ranges::any_of( diagnostics,
                                      []( const auto& diagnostic )
                                      {
                                          return diagnostic.message.find( "atspi" ) !=
                                                 std::string::npos;
                                      } ) );
}
