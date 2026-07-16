#include "client/client.hpp"
#include "client/loopback_transport.hpp"
#include "client/unix_socket_transport.hpp"
#include "grab/capture.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/event_descriptor.hpp"
#include "grab/interaction.hpp"
#include "grab/locator.hpp"
#include "grab/query.hpp"
#include "grab/result.hpp"
#include "grab/role.hpp"
#include "grab/session.hpp"
#include "grab/trace.hpp"
#include "grab/ui.hpp"
#include "screen/enumerate.hpp"
#include "transport/server.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <variant>
#include <xcb/xcb.h>
#include <xcb/xproto.h>
// clang-format on

namespace
{

    constexpr std::string_view unixEndpointPrefix = "unix:";
    constexpr std::string_view transportStartFailurePrefix =
        "failed to start transport server at unix:";

    class TempSocket
    {
        public:

            TempSocket() :
                root_( std::filesystem::temp_directory_path() /
                       ( "grab-client-test-" +
                         std::to_string( static_cast<std::uint64_t>( getpid() ) ) ) ),
                endpoint_( std::string{ unixEndpointPrefix } +
                           ( root_ / "transport.sock" ).string() )
            {
                std::error_code ec;
                static_cast<void>( std::filesystem::remove_all( root_, ec ) );
                static_cast<void>( std::filesystem::create_directories( root_, ec ) );
            }

            ~TempSocket()
            {
                std::error_code ec;
                static_cast<void>( std::filesystem::remove_all( root_, ec ) );
            }

            TempSocket( const TempSocket& ) = delete;
            TempSocket&
            operator=( const TempSocket& ) = delete;
            TempSocket( TempSocket&& )     = delete;
            TempSocket&
            operator=( TempSocket&& ) = delete;

            [[nodiscard]]
            const std::string&
            endpoint() const noexcept
            {
                return endpoint_;
            }

        private:

            std::filesystem::path root_;
            std::string           endpoint_;
    };

    TEST( ClientLoopbackTransport,
          PushDeliversToInProcessSubscriber )
    {
        grab::EventBus                  bus;
        grab::client::LoopbackTransport transport{ bus };
        grab::client::Client            client{ transport };

        auto subscription = client.subscribe( grab::EventFilter{} );
        ASSERT_TRUE( subscription.has_value() );
        auto stream = std::move( *subscription );
        ASSERT_NE( stream, nullptr );

        const grab::Event expected{
            .timestamp = 42.5,
            .sequence  = 7U,
            .kind      = grab::EventKind::KeyDown,
            .category  = grab::EventCategory::Input,
            .payload   = grab::InputKey{ .code = 30U, .name = "a" },
        };

        auto pushed = client.push_event( expected );
        ASSERT_TRUE( pushed.has_value() );

        auto next = stream->try_next();
        ASSERT_TRUE( next.has_value() );
        ASSERT_TRUE( next->has_value() );
        ASSERT_TRUE( std::holds_alternative<grab::Event>( **next ) );
        const auto& received = std::get<grab::Event>( **next );
        EXPECT_EQ( received.kind, expected.kind );
        EXPECT_EQ( received.category, expected.category );
        EXPECT_DOUBLE_EQ( received.timestamp, expected.timestamp );
        EXPECT_GT( received.sequence, 0U );
    }

    TEST( ClientLoopbackTransport,
          ListEventTypesReturnsDescriptorSet )
    {
        grab::EventBus                  bus;
        grab::client::LoopbackTransport transport{ bus };
        grab::client::Client            client{ transport };

        auto                            descriptors = client.list_event_types();
        ASSERT_TRUE( descriptors.has_value() );
        EXPECT_EQ( descriptors->size(), grab::detail::eventDescriptors.size() - 1U );

        const auto key_down = std::ranges::find( *descriptors,
                                                 grab::EventKind::KeyDown,
                                                 &grab::EventTypeDescriptor::kind );
        ASSERT_NE( key_down, descriptors->end() );
        EXPECT_EQ( key_down->category, grab::EventCategory::Input );
        EXPECT_EQ( key_down->name, "input.key_down" );
        EXPECT_FALSE( key_down->active );
    }

    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    TEST( UnixSocketTransport,
          RoundTripsThroughTransportServer )
    {
        const TempSocket temp;
        grab::EventBus   bus;
        auto server = grab::transport::TransportServer::start( temp.endpoint(), bus );
        if( !server.has_value() &&
            server.error().message.starts_with( transportStartFailurePrefix ) )
        {
            GTEST_SKIP() << server.error().message;
        }
        ASSERT_TRUE( server.has_value() ) << server.error().message;

        auto bus_subscription = bus.subscribe( grab::EventFilter{} );
        grab::client::UnixSocketTransport transport{ temp.endpoint() };
        grab::client::Client              client{ transport };

        auto                              descriptors = client.list_event_types();
        ASSERT_TRUE( descriptors.has_value() ) << descriptors.error().message;
        EXPECT_EQ( descriptors->size(), grab::detail::eventDescriptors.size() - 1U );

        const grab::Event expected{
            .timestamp = 84.25,
            .sequence  = 0U,
            .kind      = grab::EventKind::KeyDown,
            .category  = grab::EventCategory::Input,
            .payload   = grab::InputKey{ .code = 42U, .name = "shift" },
        };
        auto pushed = client.push_event( expected );
        ASSERT_TRUE( pushed.has_value() ) << pushed.error().message;

        auto received = bus_subscription.try_pop_item();
        ASSERT_TRUE( received.has_value() );
        ASSERT_TRUE( std::holds_alternative<grab::Event>( *received ) );
        const auto& event = std::get<grab::Event>( *received );
        EXPECT_EQ( event.kind, expected.kind );
        EXPECT_EQ( event.category, expected.category );
        EXPECT_DOUBLE_EQ( event.timestamp, expected.timestamp );
        EXPECT_TRUE( std::holds_alternative<grab::InputKey>( event.payload ) );
        const auto& key = std::get<grab::InputKey>( event.payload );
        EXPECT_EQ( key.code, 42U );
        EXPECT_EQ( key.name, "shift" );

        server->shutdown();
    }

    TEST( ClientErrors,
          DistinguishesConnectionsFromSemanticFailures )
    {
        const grab::Error connection{
            .code        = grab::ErrorCode::EnvironmentChanged,
            .message     = "daemon connection unavailable",
            .capability  = {},
            .target      = {},
            .attempts    = {},
            .disposition = grab::ErrorDisposition::RetrySame,
            .diagnostics = {},
        };
        const grab::Error semantic{
            .code        = grab::ErrorCode::InvalidArgument,
            .message     = "event is invalid",
            .capability  = {},
            .target      = {},
            .attempts    = {},
            .disposition = grab::ErrorDisposition::Fatal,
            .diagnostics = {},
        };

        EXPECT_TRUE( grab::client::is_connection_error( connection ) );
        EXPECT_FALSE( grab::client::is_connection_error( semantic ) );
    }

    constexpr std::string_view loopbackWindowTitle       = "grab loopback verbs";
    constexpr std::string_view loopbackWindowClass       = "grab-loopback-verbs";
    constexpr std::string_view socketWindowTitle         = "grab socket verbs";
    constexpr std::string_view socketWindowClass         = "grab-socket-verbs";
    constexpr std::string_view utf8StringAtomName        = "UTF8_STRING";
    constexpr std::string_view netWmNameAtomName         = "_NET_WM_NAME";
    constexpr std::string_view netClientListAtomName     = "_NET_CLIENT_LIST";
    constexpr std::int16_t     loopbackWindowX           = 11;
    constexpr std::int16_t     loopbackWindowY           = 13;
    constexpr std::uint16_t    loopbackWindowWidth       = 200U;
    constexpr std::uint16_t    loopbackWindowHeight      = 120U;
    constexpr std::uint16_t    loopbackWindowBorderWidth = 0U;
    constexpr std::size_t      loopbackWindowValueCount  = 2U;
    constexpr std::uint32_t    loopbackWindowValueMask =
        XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    constexpr std::uint32_t loopbackWindowEventMask = XCB_EVENT_MASK_EXPOSURE |
                                                      XCB_EVENT_MASK_STRUCTURE_NOTIFY |
                                                      XCB_EVENT_MASK_PROPERTY_CHANGE;
    constexpr std::uint8_t  windowPropertyFormat    = 32U;
    constexpr std::uint32_t clientListWindowCount   = 1U;
    constexpr int           xcbConnectionSuccess    = 0;
    constexpr int           xcbFlushFailure         = 0;
    constexpr std::uint64_t emptyFrameId            = 0U;

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

    TEST( ClientLoopbackVerbs,
          VerbsWithoutBoundSessionReturnCapabilityUnavailable )
    {
        grab::EventBus                  bus;
        grab::client::LoopbackTransport transport{ bus };
        grab::client::Client            client{ transport };

        const auto match = client.resolve( grab::sel::role( grab::role::window ) );
        ASSERT_FALSE( match.has_value() );
        EXPECT_EQ( match.error().code, grab::ErrorCode::CapabilityUnavailable );

        const auto receipt = client.perform(
            grab::Click{ .target = grab::sel::role( grab::role::window ) },
            grab::ActionOptions{}
        );
        ASSERT_FALSE( receipt.has_value() );
        EXPECT_EQ( receipt.error().code, grab::ErrorCode::CapabilityUnavailable );

        const auto frame =
            client.capture( grab::CaptureTarget{ std::string{ "screen" } },
                            grab::CaptureOptions{} );
        ASSERT_FALSE( frame.has_value() );
        EXPECT_EQ( frame.error().code, grab::ErrorCode::CapabilityUnavailable );
    }

    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    TEST( ClientLoopbackVerbs,
          VerbsRoundTripAgainstLiveSessionOnDisplay )
    {
        const char* const display = std::getenv( "DISPLAY" );
        if( display == nullptr || std::string_view{ display }.empty() )
        {
            GTEST_SKIP() << "requires Xvfb (DISPLAY is not set)";
        }

        int            screenNumber = 0;
        XcbWindowGuard windowGuard;
        windowGuard.connection = xcb_connect( display, &screenNumber );
        if( windowGuard.connection ==
            nullptr ||
            xcb_connection_has_error( windowGuard.connection ) != xcbConnectionSuccess )
        {
            GTEST_SKIP() << "requires a reachable X display";
        }

        auto screenIterator =
            xcb_setup_roots_iterator( xcb_get_setup( windowGuard.connection ) );
        for( int screenIndex = 0; screenIndex < screenNumber && screenIterator.rem > 0;
             ++screenIndex )
        {
            xcb_screen_next( &screenIterator );
        }
        ASSERT_GT( screenIterator.rem, 0 );
        ASSERT_NE( screenIterator.data, nullptr );
        const auto* const screen = screenIterator.data;

        windowGuard.window       = xcb_generate_id( windowGuard.connection );
        const std::array<std::uint32_t, loopbackWindowValueCount> windowValues{
            screen->black_pixel,
            loopbackWindowEventMask,
        };
        ASSERT_TRUE(
            request_succeeded( windowGuard.connection,
                               xcb_create_window_checked( windowGuard.connection,
                                                          XCB_COPY_FROM_PARENT,
                                                          windowGuard.window,
                                                          screen->root,
                                                          loopbackWindowX,
                                                          loopbackWindowY,
                                                          loopbackWindowWidth,
                                                          loopbackWindowHeight,
                                                          loopbackWindowBorderWidth,
                                                          XCB_WINDOW_CLASS_INPUT_OUTPUT,
                                                          screen->root_visual,
                                                          loopbackWindowValueMask,
                                                          windowValues.data() ) )
        );

        const auto utf8StringAtom =
            intern_atom( windowGuard.connection, utf8StringAtomName );
        const auto netWmNameAtom =
            intern_atom( windowGuard.connection, netWmNameAtomName );
        const auto netClientListAtom =
            intern_atom( windowGuard.connection, netClientListAtomName );
        ASSERT_NE( utf8StringAtom, XCB_ATOM_NONE );
        ASSERT_NE( netWmNameAtom, XCB_ATOM_NONE );
        ASSERT_NE( netClientListAtom, XCB_ATOM_NONE );

        ASSERT_TRUE( set_text_property( windowGuard.connection,
                                        windowGuard.window,
                                        XCB_ATOM_WM_NAME,
                                        XCB_ATOM_STRING,
                                        loopbackWindowTitle ) );
        ASSERT_TRUE( set_text_property( windowGuard.connection,
                                        windowGuard.window,
                                        netWmNameAtom,
                                        utf8StringAtom,
                                        loopbackWindowTitle ) );
        const std::string windowClassValue = std::string{ loopbackWindowClass } +
                                             '\0' +
                                             std::string{ loopbackWindowClass } +
                                             '\0';
        ASSERT_TRUE( set_text_property( windowGuard.connection,
                                        windowGuard.window,
                                        XCB_ATOM_WM_CLASS,
                                        XCB_ATOM_STRING,
                                        windowClassValue ) );
        ASSERT_TRUE( request_succeeded( windowGuard.connection,
                                        xcb_map_window_checked( windowGuard.connection,
                                                                windowGuard.window ) ) );
        ASSERT_TRUE(
            request_succeeded( windowGuard.connection,
                               xcb_change_property_checked( windowGuard.connection,
                                                            XCB_PROP_MODE_REPLACE,
                                                            screen->root,
                                                            netClientListAtom,
                                                            XCB_ATOM_WINDOW,
                                                            windowPropertyFormat,
                                                            clientListWindowCount,
                                                            &windowGuard.window ) )
        );
        ASSERT_GT( xcb_flush( windowGuard.connection ), xcbFlushFailure );

        auto session = grab::Session::open( grab::SessionOptions{
            .display = std::string{ display },
            .seat    = {},
        } );
        ASSERT_TRUE( session.has_value() ) << session.error().message;
        grab::client::LoopbackTransport transport{ std::move( *session ) };
        grab::client::Client            client{ transport };

        const auto                      locator = grab::sel::all(
            { grab::sel::role( grab::role::window ),
              grab::sel::property( grab::property::title,
                                   std::string{ loopbackWindowTitle } ),
              grab::sel::property( grab::property::window_class,
                                   std::string{ loopbackWindowClass } ) }
        );
        const auto match = client.resolve( locator );
        ASSERT_TRUE( match.has_value() ) << match.error().message;

        const auto outputs = grab::screen::list_outputs();
        ASSERT_TRUE( outputs.has_value() );
        ASSERT_FALSE( outputs->empty() );
        const auto frame = client.capture( grab::CaptureTarget{ outputs->front().name },
                                           grab::CaptureOptions{} );
        ASSERT_TRUE( frame.has_value() ) << frame.error().message;
        EXPECT_NE( frame->id.value, emptyFrameId );

        const auto receipt =
            client.perform( grab::Click{ .target = *match }, grab::ActionOptions{} );
        ASSERT_TRUE( receipt.has_value() ) << receipt.error().message;
        EXPECT_TRUE( receipt->commit ==
                     grab::CommitStatus::Committed ||
                     receipt->commit == grab::CommitStatus::Verified );
        EXPECT_FALSE( receipt->routes.empty() );
    }

    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    TEST( ClientSocketVerbs,
          VerbsRoundTripThroughTransportServer )
    {
        const char* const display = std::getenv( "DISPLAY" );
        if( display == nullptr || std::string_view{ display }.empty() )
        {
            GTEST_SKIP() << "requires Xvfb (DISPLAY is not set)";
        }

        int            screenNumber = 0;
        XcbWindowGuard windowGuard;
        windowGuard.connection = xcb_connect( display, &screenNumber );
        if( windowGuard.connection ==
            nullptr ||
            xcb_connection_has_error( windowGuard.connection ) != xcbConnectionSuccess )
        {
            GTEST_SKIP() << "requires a reachable X display";
        }

        auto screenIterator =
            xcb_setup_roots_iterator( xcb_get_setup( windowGuard.connection ) );
        for( int screenIndex = 0; screenIndex < screenNumber && screenIterator.rem > 0;
             ++screenIndex )
        {
            xcb_screen_next( &screenIterator );
        }
        ASSERT_GT( screenIterator.rem, 0 );
        ASSERT_NE( screenIterator.data, nullptr );
        const auto* const screen = screenIterator.data;

        windowGuard.window       = xcb_generate_id( windowGuard.connection );
        const std::array<std::uint32_t, loopbackWindowValueCount> windowValues{
            screen->black_pixel,
            loopbackWindowEventMask,
        };
        ASSERT_TRUE(
            request_succeeded( windowGuard.connection,
                               xcb_create_window_checked( windowGuard.connection,
                                                          XCB_COPY_FROM_PARENT,
                                                          windowGuard.window,
                                                          screen->root,
                                                          loopbackWindowX,
                                                          loopbackWindowY,
                                                          loopbackWindowWidth,
                                                          loopbackWindowHeight,
                                                          loopbackWindowBorderWidth,
                                                          XCB_WINDOW_CLASS_INPUT_OUTPUT,
                                                          screen->root_visual,
                                                          loopbackWindowValueMask,
                                                          windowValues.data() ) )
        );

        const auto utf8StringAtom =
            intern_atom( windowGuard.connection, utf8StringAtomName );
        const auto netWmNameAtom =
            intern_atom( windowGuard.connection, netWmNameAtomName );
        const auto netClientListAtom =
            intern_atom( windowGuard.connection, netClientListAtomName );
        ASSERT_NE( utf8StringAtom, XCB_ATOM_NONE );
        ASSERT_NE( netWmNameAtom, XCB_ATOM_NONE );
        ASSERT_NE( netClientListAtom, XCB_ATOM_NONE );

        ASSERT_TRUE( set_text_property( windowGuard.connection,
                                        windowGuard.window,
                                        XCB_ATOM_WM_NAME,
                                        XCB_ATOM_STRING,
                                        socketWindowTitle ) );
        ASSERT_TRUE( set_text_property( windowGuard.connection,
                                        windowGuard.window,
                                        netWmNameAtom,
                                        utf8StringAtom,
                                        socketWindowTitle ) );
        const std::string windowClassValue = std::string{ socketWindowClass } +
                                             '\0' +
                                             std::string{ socketWindowClass } +
                                             '\0';
        ASSERT_TRUE( set_text_property( windowGuard.connection,
                                        windowGuard.window,
                                        XCB_ATOM_WM_CLASS,
                                        XCB_ATOM_STRING,
                                        windowClassValue ) );
        ASSERT_TRUE( request_succeeded( windowGuard.connection,
                                        xcb_map_window_checked( windowGuard.connection,
                                                                windowGuard.window ) ) );
        ASSERT_TRUE(
            request_succeeded( windowGuard.connection,
                               xcb_change_property_checked( windowGuard.connection,
                                                            XCB_PROP_MODE_REPLACE,
                                                            screen->root,
                                                            netClientListAtom,
                                                            XCB_ATOM_WINDOW,
                                                            windowPropertyFormat,
                                                            clientListWindowCount,
                                                            &windowGuard.window ) )
        );
        ASSERT_GT( xcb_flush( windowGuard.connection ), xcbFlushFailure );

        auto session = grab::Session::open( grab::SessionOptions{
            .display = std::string{ display },
            .seat    = {},
        } );
        ASSERT_TRUE( session.has_value() ) << session.error().message;

        const TempSocket temp;
        grab::EventBus   bus;
        auto server = grab::transport::TransportServer::start( temp.endpoint(),
                                                               bus,
                                                               nullptr,
                                                               session->get() );
        if( !server.has_value() &&
            server.error().message.starts_with( transportStartFailurePrefix ) )
        {
            GTEST_SKIP() << server.error().message;
        }
        ASSERT_TRUE( server.has_value() ) << server.error().message;

        grab::client::UnixSocketTransport transport{ temp.endpoint() };
        grab::client::Client              client{ transport };

        const auto                        locator = grab::sel::all(
            { grab::sel::role( grab::role::window ),
              grab::sel::property( grab::property::title,
                                   std::string{ socketWindowTitle } ),
              grab::sel::property( grab::property::window_class,
                                   std::string{ socketWindowClass } ) }
        );
        const auto match = client.resolve( locator );
        ASSERT_TRUE( match.has_value() ) << match.error().message;
        EXPECT_NE( match->ref.node, 0U );

        const auto outputs = grab::screen::list_outputs();
        ASSERT_TRUE( outputs.has_value() );
        ASSERT_FALSE( outputs->empty() );
        const auto frame = client.capture( grab::CaptureTarget{ outputs->front().name },
                                           grab::CaptureOptions{} );
        ASSERT_TRUE( frame.has_value() ) << frame.error().message;
        EXPECT_NE( frame->id.value, 0U );
        EXPECT_GT( frame->image.width, 0U );
        EXPECT_GT( frame->image.height, 0U );

        const auto receipt =
            client.perform( grab::Click{ .target = *match }, grab::ActionOptions{} );
        ASSERT_FALSE( receipt.has_value() );
        EXPECT_EQ( receipt.error().code, grab::ErrorCode::PermissionNeeded );

        server->shutdown();
    }

}    // namespace
