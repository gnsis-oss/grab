#include "drivers/desktop/x11/enumerate.hpp"
#include "drivers/desktop/x11/protocol.hpp"
#include "drivers/desktop/x11/window_match.hpp"
#include "drivers/desktop/x11/x11_capture_route.hpp"
#include "grab/capture.hpp"
#include "grab/image.hpp"
#include "grab/result.hpp"
#include "grab/screen.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <xcb/xcb.h>
#include <xcb/xproto.h>

namespace grab
{
    namespace
    {

        constexpr int           xcbOk                      = 0;
        constexpr std::uint8_t  x11SuccessResponse         = 1U;
        constexpr std::uint8_t  format32Bits               = 32U;
        constexpr std::uint32_t propertyOffsetZero         = 0U;
        constexpr std::uint32_t singleWindowPropertyLength = 1U;
        constexpr std::uint8_t  atomOnlyIfExists           = 1U;
        constexpr std::uint8_t  atomCreateIfMissing        = 0U;
        // EWMH _NET_ACTIVE_WINDOW data32: source indication 1 == "normal
        // application", then the timestamp, then the window being replaced.
        constexpr std::uint32_t activationSourceNormal    = 1U;
        constexpr std::uint32_t activationNoCurrentWindow = 0U;
        constexpr std::uint8_t  messageNoPropagate        = 0U;
        constexpr std::uint32_t rootMessageEventMask =
            XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT;
        constexpr std::uint32_t activationStackAbove = XCB_STACK_MODE_ABOVE;

        // Every EWMH root message carries exactly five 32-bit words.
        constexpr std::size_t   clientMessageWordCount = 5U;
        using ClientMessagePayload = std::array<std::uint32_t, clientMessageWordCount>;

        // _NET_WM_STATE action code for "remove this state".
        constexpr std::uint32_t stateActionRemove  = 0U;

        constexpr std::size_t   geometryValueCount = 4U;
        constexpr std::uint16_t geometryValueMask  = XCB_CONFIG_WINDOW_X |
                                                     XCB_CONFIG_WINDOW_Y |
                                                     XCB_CONFIG_WINDOW_WIDTH |
                                                     XCB_CONFIG_WINDOW_HEIGHT;

        // _NET_MOVERESIZE_WINDOW packs its flags into the first word: the gravity
        // in the low byte, one presence bit per supplied field, then the source
        // indication. Static gravity anchors on the client window's own corner.
        constexpr std::uint32_t moveresizeXPresent      = 1U << 8U;
        constexpr std::uint32_t moveresizeYPresent      = 1U << 9U;
        constexpr std::uint32_t moveresizeWidthPresent  = 1U << 10U;
        constexpr std::uint32_t moveresizeHeightPresent = 1U << 11U;
        constexpr std::uint32_t moveresizeSourceNormal  = 1U << 12U;
        constexpr std::uint32_t moveresizeFlags =
            static_cast<std::uint32_t>( XCB_GRAVITY_STATIC ) |
            moveresizeXPresent |
            moveresizeYPresent |
            moveresizeWidthPresent |
            moveresizeHeightPresent |
            moveresizeSourceNormal;

        // A window manager may resize a window asynchronously right after it maps,
        // so a placement only counts as settled once the requested geometry has
        // survived this many consecutive reads spaced this far apart.
        constexpr std::size_t settledObservationCount = 3U;
        constexpr auto        placementPollInterval   = std::chrono::milliseconds{ 40 };

        template<typename T>
        using XcbOwned = std::unique_ptr<T, decltype( &std::free )>;

        using XcbConnection =
            std::unique_ptr<xcb_connection_t, decltype( &xcb_disconnect )>;

        [[nodiscard]]
        grab::Result<grab::Image>
        image_of( grab::Result<grab::Frame> frame )
        {
            if( !frame.has_value() )
            {
                return std::unexpected( std::move( frame.error() ) );
            }
            return std::move( frame->image );
        }

        struct ActiveDisplay
        {
                XcbConnection connection;
                xcb_window_t  root = XCB_WINDOW_NONE;
        };

        template<typename T>
        [[nodiscard]]
        XcbOwned<T>
        take_xcb_owned( T* pointer ) noexcept
        {
            return XcbOwned<T>{ pointer, &std::free };
        }

        [[nodiscard]]
        grab::Result<ActiveDisplay>
        connect_active_display( const char* display )
        {
            int           screen_index = 0;
            XcbConnection connection{
                xcb_connect( display, &screen_index ),
                &xcb_disconnect
            };
            if( connection ==
                nullptr ||
                xcb_connection_has_error( connection.get() ) != xcbOk )
            {
                return grab::fail( grab::ErrorCode::DeviceInaccessible,
                                   "XCB display connection failed" );
            }

            const xcb_setup_t* const setup = xcb_get_setup( connection.get() );
            if( setup == nullptr || setup->status != x11SuccessResponse )
            {
                return grab::fail( grab::ErrorCode::DeviceInaccessible,
                                   "XCB setup is unavailable" );
            }

            xcb_screen_iterator_t iterator = xcb_setup_roots_iterator( setup );
            for( int current_screen = 0;
                 current_screen < screen_index && iterator.rem > 0;
                 ++current_screen )
            {
                xcb_screen_next( &iterator );
            }

            if( iterator.data == nullptr )
            {
                return grab::fail( grab::ErrorCode::DeviceInaccessible,
                                   "XCB default screen is unavailable" );
            }

            return ActiveDisplay{
                .connection = std::move( connection ),
                .root       = iterator.data->root,
            };
        }

        // `only_if_exists` distinguishes the two uses: reading a property must not
        // fabricate an atom the desktop never published (a miss is a real "no
        // window manager" answer), whereas sending a client message needs the atom
        // to exist even on a bare display so the request can at least be delivered.
        [[nodiscard]]
        grab::Result<xcb_atom_t>
        intern_atom( xcb_connection_t* connection,
                     std::string_view  name,
                     std::uint8_t      only_if_exists )
        {
            xcb_generic_error_t* raw_error = nullptr;
            const auto           reply     = take_xcb_owned( xcb_intern_atom_reply(
                connection,
                xcb_intern_atom( connection,
                                 only_if_exists,
                                 static_cast<std::uint16_t>( name.size() ),
                                 name.data() ),
                &raw_error
            ) );
            const auto           error     = take_xcb_owned( raw_error );
            if( error != nullptr || reply == nullptr )
            {
                return grab::fail( grab::ErrorCode::ProtocolError,
                                   "XCB atom lookup failed for " + std::string{ name } );
            }
            if( reply->atom == XCB_ATOM_NONE )
            {
                return grab::fail( grab::ErrorCode::WindowNotFound,
                                   "EWMH atom is unavailable: " + std::string{ name } );
            }
            return reply->atom;
        }

        [[nodiscard]]
        grab::Result<xcb_atom_t>
        intern_active_window_atom( xcb_connection_t* connection )
        {
            return intern_atom( connection,
                                grab::platform::x11::atom_name::netActiveWindow,
                                atomOnlyIfExists );
        }

        // EWMH root-window messages are how a client asks the window manager to do
        // something to another window; the WM selects SubstructureRedirect on the
        // root, so the message must be addressed there rather than to the target.
        [[nodiscard]]
        grab::Result<void>
        send_root_message( xcb_connection_t*           connection,
                           xcb_window_t                root,
                           xcb_window_t                window,
                           std::string_view            type_name,
                           const ClientMessagePayload& payload,
                           std::string_view            description )
        {
            auto atom = intern_atom( connection, type_name, atomCreateIfMissing );
            if( !atom.has_value() )
            {
                return std::unexpected( std::move( atom.error() ) );
            }

            xcb_client_message_event_t event{
                .response_type = XCB_CLIENT_MESSAGE,
                .format        = format32Bits,
                .sequence      = 0U,
                .window        = window,
                .type          = *atom,
                .data          = xcb_client_message_data_t{},
            };
            std::ranges::copy( payload, std::begin( event.data.data32 ) );

            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            const auto* const raw_event  = reinterpret_cast<const char*>( &event );
            const auto        send_error = take_xcb_owned(
                xcb_request_check( connection,
                                   xcb_send_event_checked( connection,
                                                           messageNoPropagate,
                                                           root,
                                                           rootMessageEventMask,
                                                           raw_event ) )
            );
            if( send_error != nullptr )
            {
                return grab::fail( grab::ErrorCode::ProtocolError,
                                   "XCB request failed: " + std::string{ description } );
            }
            return {};
        }

        // Asks the window manager to focus `window` via EWMH, then unconditionally
        // raises it directly. The direct raise is the fallback that makes this work
        // on displays with no (or an uncooperative) window manager; the client
        // message alone is a no-op there because nobody is listening on the root.
        [[nodiscard]]
        grab::Result<void>
        raise_and_focus( xcb_connection_t* connection,
                         xcb_window_t      root,
                         xcb_window_t      window )
        {
            auto sent =
                send_root_message( connection,
                                   root,
                                   window,
                                   grab::platform::x11::atom_name::netActiveWindow,
                                   ClientMessagePayload{
                                       activationSourceNormal,
                                       XCB_CURRENT_TIME,
                                       activationNoCurrentWindow,
                                       0U,
                                       0U
                                   },
                                   "active-window" );
            if( !sent.has_value() )
            {
                return sent;
            }

            const auto raise_error = take_xcb_owned( xcb_request_check(
                connection,
                xcb_configure_window_checked( connection,
                                              window,
                                              XCB_CONFIG_WINDOW_STACK_MODE,
                                              &activationStackAbove )
            ) );
            if( raise_error != nullptr )
            {
                return grab::fail( grab::ErrorCode::ProtocolError,
                                   "XCB window raise request failed" );
            }

            if( xcb_flush( connection ) <= 0 )
            {
                return grab::fail( grab::ErrorCode::ProtocolError,
                                   "XCB window activation flush failed" );
            }
            return {};
        }

        // Drops the states that make a window manager ignore geometry requests.
        // Both messages are best-effort: a window that is not maximised, and a
        // display with no window manager at all, simply ignore them.
        [[nodiscard]]
        grab::Result<void>
        clear_maximised_state( xcb_connection_t* connection,
                               xcb_window_t      root,
                               xcb_window_t      window )
        {
            namespace atom_name = grab::platform::x11::atom_name;

            auto vertical       = intern_atom( connection,
                                               atom_name::netWmStateMaximizedVert,
                                               atomCreateIfMissing );
            if( !vertical.has_value() )
            {
                return std::unexpected( std::move( vertical.error() ) );
            }
            auto horizontal = intern_atom( connection,
                                           atom_name::netWmStateMaximizedHorz,
                                           atomCreateIfMissing );
            if( !horizontal.has_value() )
            {
                return std::unexpected( std::move( horizontal.error() ) );
            }
            auto fullscreen = intern_atom( connection,
                                           atom_name::netWmStateFullscreen,
                                           atomCreateIfMissing );
            if( !fullscreen.has_value() )
            {
                return std::unexpected( std::move( fullscreen.error() ) );
            }

            // _NET_WM_STATE carries at most two properties per message, so the
            // two maximise axes travel together and fullscreen follows alone.
            auto unmaximised = send_root_message( connection,
                                                  root,
                                                  window,
                                                  atom_name::netWmState,
                                                  ClientMessagePayload{
                                                      stateActionRemove,
                                                      *vertical,
                                                      *horizontal,
                                                      activationSourceNormal,
                                                      0U
                                                  },
                                                  "un-maximise" );
            if( !unmaximised.has_value() )
            {
                return unmaximised;
            }

            auto windowed = send_root_message( connection,
                                               root,
                                               window,
                                               atom_name::netWmState,
                                               ClientMessagePayload{
                                                   stateActionRemove,
                                                   *fullscreen,
                                                   0U,
                                                   activationSourceNormal,
                                                   0U
                                               },
                                               "leave-fullscreen" );
            if( !windowed.has_value() )
            {
                return windowed;
            }

            if( xcb_flush( connection ) <= 0 )
            {
                return grab::fail( grab::ErrorCode::ProtocolError,
                                   "XCB window state flush failed" );
            }
            return {};
        }

        // Requests `bounds` twice over, by two mechanisms that cover the two worlds
        // this runs in. The plain ConfigureWindow is what a display with no window
        // manager honours directly; _NET_MOVERESIZE_WINDOW is what an EWMH window
        // manager honours, and it is sent second so it wins where both apply. Static
        // gravity makes its coordinates mean the client window's own top-left in
        // root space, which is exactly the frame windows() reports — without it a
        // decorated window would land a title-bar's height off.
        [[nodiscard]]
        grab::Result<void>
        request_geometry( xcb_connection_t*                connection,
                          xcb_window_t                     root,
                          xcb_window_t                     window,
                          const grab::geometry::Rectangle& bounds )
        {
            const std::array<std::uint32_t, geometryValueCount> values{
                static_cast<std::uint32_t>( bounds.x ),
                static_cast<std::uint32_t>( bounds.y ),
                bounds.width,
                bounds.height,
            };
            const auto configure_error = take_xcb_owned(
                xcb_request_check( connection,
                                   xcb_configure_window_checked( connection,
                                                                 window,
                                                                 geometryValueMask,
                                                                 values.data() ) )
            );
            if( configure_error != nullptr )
            {
                return grab::fail( grab::ErrorCode::ProtocolError,
                                   "XCB window configure request failed" );
            }

            auto moved =
                send_root_message( connection,
                                   root,
                                   window,
                                   grab::platform::x11::atom_name::netMoveresizeWindow,
                                   ClientMessagePayload{
                                       moveresizeFlags,
                                       static_cast<std::uint32_t>( bounds.x ),
                                       static_cast<std::uint32_t>( bounds.y ),
                                       bounds.width,
                                       bounds.height
                                   },
                                   "move-resize" );
            if( !moved.has_value() )
            {
                return moved;
            }

            if( xcb_flush( connection ) <= 0 )
            {
                return grab::fail( grab::ErrorCode::ProtocolError,
                                   "XCB window geometry flush failed" );
            }
            return {};
        }

        // timerfd rather than a thread sleep so the poll cadence stays a real
        // kernel timer that a future reactor can own, matching how the batch and
        // watch runners pace themselves.
        class PollTimer
        {
            public:

                PollTimer() noexcept :
                    descriptor_( ::timerfd_create( CLOCK_MONOTONIC,
                                                   TFD_CLOEXEC ) )
                {
                }

                ~PollTimer() noexcept
                {
                    if( descriptor_ != invalidDescriptor )
                    {
                        static_cast<void>( ::close( descriptor_ ) );
                    }
                }

                PollTimer( const PollTimer& ) = delete;
                PollTimer&
                operator=( const PollTimer& ) = delete;
                PollTimer( PollTimer&& )      = delete;
                PollTimer&
                operator=( PollTimer&& ) = delete;

                [[nodiscard]]
                grab::Result<void>
                wait_for( std::chrono::milliseconds duration ) const
                {
                    if( descriptor_ == invalidDescriptor )
                    {
                        return grab::fail( grab::ErrorCode::ProviderFailed,
                                           "timerfd_create failed" );
                    }

                    const auto nanoseconds =
                        std::chrono::duration_cast<std::chrono::nanoseconds>( duration );
                    itimerspec timer{};
                    timer.it_value.tv_nsec =
                        static_cast<decltype( timer.it_value.tv_nsec )>(
                            nanoseconds.count()
                        );
                    if( ::timerfd_settime( descriptor_,
                                           0,
                                           std::addressof( timer ),
                                           nullptr ) != 0 )
                    {
                        return grab::fail( grab::ErrorCode::ProviderFailed,
                                           "timerfd_settime failed" );
                    }

                    std::uint64_t expirations = 0U;
                    const auto    read_bytes  = ::read( descriptor_,
                                                        std::addressof( expirations ),
                                                        sizeof( expirations ) );
                    if( read_bytes != static_cast<ssize_t>( sizeof( expirations ) ) )
                    {
                        return grab::fail( grab::ErrorCode::ProviderFailed,
                                           "timerfd read failed" );
                    }
                    return {};
                }

            private:

                static constexpr int invalidDescriptor = -1;

                int                  descriptor_       = invalidDescriptor;
        };

        [[nodiscard]]
        bool
        bounds_match( const grab::geometry::Rectangle& observed,
                      const grab::geometry::Rectangle& request ) noexcept
        {
            return observed.x ==
                   request.x &&
                   observed.y ==
                   request.y &&
                   observed.width ==
                   request.width &&
                   observed.height == request.height;
        }

        [[nodiscard]]
        std::string
        describe_bounds( const grab::geometry::Rectangle& bounds )
        {
            return std::to_string( bounds.width ) +
                   "x" +
                   std::to_string( bounds.height ) +
                   "+" +
                   std::to_string( bounds.x ) +
                   "+" +
                   std::to_string( bounds.y );
        }

        // Succeeds only once the window has reported the requested geometry on
        // `settledObservationCount` consecutive reads; any deviation restarts the
        // count, so a window manager that re-maximises mid-wait cannot be mistaken
        // for a settled placement.
        [[nodiscard]]
        grab::Result<grab::geometry::Rectangle>
        await_placement( xcb_connection_t*                connection,
                         xcb_window_t                     root,
                         xcb_window_t                     window,
                         const grab::geometry::Rectangle& request,
                         std::chrono::milliseconds        timeout )
        {
            const PollTimer timer;
            const auto      deadline = std::chrono::steady_clock::now() + timeout;
            std::size_t     settled  = 0U;
            grab::geometry::Rectangle observed{};

            for( ;; )
            {
                auto bounds = grab::screen::window_bounds( connection, root, window );
                if( !bounds.has_value() )
                {
                    return std::unexpected( std::move( bounds.error() ) );
                }
                observed = *bounds;
                settled  = bounds_match( observed, request ) ? settled + 1U : 0U;
                if( settled >= settledObservationCount )
                {
                    return observed;
                }

                if( std::chrono::steady_clock::now() >= deadline )
                {
                    return grab::fail( grab::ErrorCode::DeadlineExceeded,
                                       "window did not settle on the requested "
                                       "geometry: wanted " +
                                           describe_bounds( request ) +
                                           ", observed " +
                                           describe_bounds( observed ) );
                }
                auto waited = timer.wait_for( placementPollInterval );
                if( !waited.has_value() )
                {
                    return std::unexpected( std::move( waited.error() ) );
                }
            }
        }

        [[nodiscard]]
        grab::Result<xcb_window_t>
        read_active_window_property( xcb_connection_t* connection,
                                     xcb_window_t      root,
                                     xcb_atom_t        active_window_atom )
        {
            xcb_generic_error_t* raw_error = nullptr;
            const auto           reply     = take_xcb_owned(
                xcb_get_property_reply( connection,
                                        xcb_get_property( connection,
                                                          0U,
                                                          root,
                                                          active_window_atom,
                                                          XCB_ATOM_WINDOW,
                                                          propertyOffsetZero,
                                                          singleWindowPropertyLength ),
                                        &raw_error )
            );
            const auto error = take_xcb_owned( raw_error );
            if( error != nullptr )
            {
                return grab::fail( grab::ErrorCode::ProtocolError,
                                   "XCB active window property read failed" );
            }
            if( reply ==
                nullptr ||
                reply->type ==
                XCB_ATOM_NONE ||
                xcb_get_property_value_length( reply.get() ) == 0 )
            {
                return grab::fail( grab::ErrorCode::WindowNotFound,
                                   "EWMH active window is unset" );
            }
            if( reply->format != format32Bits )
            {
                return grab::fail( grab::ErrorCode::ProtocolError,
                                   "EWMH active window property has an invalid format" );
            }

            const int value_length = xcb_get_property_value_length( reply.get() );
            if( std::cmp_less( value_length, sizeof( xcb_window_t ) ) )
            {
                return grab::fail( grab::ErrorCode::ProtocolError,
                                   "EWMH active window property is truncated" );
            }

            const auto* const bytes =
                static_cast<const std::byte*>( xcb_get_property_value( reply.get() ) );
            std::array<std::byte, sizeof( xcb_window_t )> raw_window{};
            const auto                                    window_bytes =
                std::span<const std::byte>{ bytes, raw_window.size() };
            std::ranges::copy( window_bytes, raw_window.begin() );

            const auto window = std::bit_cast<xcb_window_t>( raw_window );
            if( window == XCB_WINDOW_NONE )
            {
                return grab::fail( grab::ErrorCode::WindowNotFound,
                                   "EWMH active window is unset" );
            }
            return window;
        }

        [[nodiscard]]
        grab::Result<xcb_window_t>
        read_active_window_id( const char* display )
        {
            auto active_display = connect_active_display( display );
            if( !active_display.has_value() )
            {
                return std::unexpected( std::move( active_display.error() ) );
            }

            auto atom = intern_active_window_atom( active_display->connection.get() );
            if( !atom.has_value() )
            {
                return std::unexpected( std::move( atom.error() ) );
            }

            return read_active_window_property( active_display->connection.get(),
                                                active_display->root,
                                                *atom );
        }

    }    // namespace

    struct Screen::Impl
    {
            grab::drivers::desktop::x11::X11CaptureRoute route;
            std::optional<std::string>                   display;

            Impl( grab::drivers::desktop::x11::X11CaptureRoute route_value,
                  const char*                                  display_value ) :
                route( std::move( route_value ) ),
                display( display_value == nullptr ? std::optional<std::string>{}
                                                  : std::optional<std::string>{
                                                        std::string{ display_value }
                                                    } )
            {
            }

            [[nodiscard]]
            const char*
            display_name() const noexcept
            {
                if( display.has_value() )
                {
                    return display->c_str();
                }
                return nullptr;
            }
    };

    Screen::Screen( std::unique_ptr<Impl> impl ) noexcept :
        impl_( std::move( impl ) )
    {
    }

    Screen::~Screen()                         = default;

    Screen::Screen( Screen&& other ) noexcept = default;

    Screen&
    Screen::operator=( Screen&& other ) noexcept = default;

    grab::Result<Screen>
    Screen::open( const char* display )
    {
        auto route = grab::drivers::desktop::x11::X11CaptureRoute::open( display );
        if( !route.has_value() )
        {
            return std::unexpected( std::move( route.error() ) );
        }
        return Screen{ std::make_unique<Impl>( std::move( *route ), display ) };
    }

    grab::Result<Image>
    Screen::window_by_class( const std::vector<std::string>& wm_class_candidates )
    {
        if( impl_ == nullptr )
        {
            return grab::fail( grab::ErrorCode::InternalFault, "Screen is not open" );
        }
        const std::vector<std::string> candidates =
            grab::screen::normalized_wm_class_candidates( wm_class_candidates );
        if( candidates.empty() )
        {
            return grab::fail( grab::ErrorCode::WindowNotFound,
                               "no WM_CLASS candidates were provided" );
        }

        auto windows = screen::list_windows( impl_->display_name() );
        if( !windows.has_value() )
        {
            return std::unexpected( std::move( windows.error() ) );
        }

        for( const screen::WindowInfo& info : *windows )
        {
            if( grab::screen::wm_class_matches_any( info.wm_class, candidates ) )
            {
                return image_of( impl_->route.capture_window( info.id ) );
            }
        }

        return grab::fail( grab::ErrorCode::WindowNotFound,
                           "no window matched the requested WM_CLASS" );
    }

    grab::Result<Image>
    Screen::display()
    {
        if( impl_ == nullptr )
        {
            return grab::fail( grab::ErrorCode::InternalFault, "Screen is not open" );
        }
        return image_of( impl_->route.capture_display() );
    }

    grab::Result<Image>
    Screen::region( std::int16_t  x,
                    std::int16_t  y,
                    std::uint16_t width,
                    std::uint16_t height )
    {
        if( impl_ == nullptr )
        {
            return grab::fail( grab::ErrorCode::InternalFault, "Screen is not open" );
        }
        return image_of( impl_->route.capture_region( x, y, width, height ) );
    }

    grab::Result<Image>
    Screen::active_window()
    {
        if( impl_ == nullptr )
        {
            return grab::fail( grab::ErrorCode::InternalFault, "Screen is not open" );
        }
        auto active_window_id = read_active_window_id( impl_->display_name() );
        if( !active_window_id.has_value() )
        {
            return std::unexpected( std::move( active_window_id.error() ) );
        }

        return image_of( impl_->route.capture_window( *active_window_id ) );
    }

    grab::Result<std::vector<WindowSummary>>
    Screen::windows()
    {
        if( impl_ == nullptr )
        {
            return grab::fail( grab::ErrorCode::InternalFault, "Screen is not open" );
        }

        auto listed = screen::list_windows( impl_->display_name() );
        if( !listed.has_value() )
        {
            return std::unexpected( std::move( listed.error() ) );
        }

        std::vector<WindowSummary> result;
        result.reserve( listed->size() );
        for( screen::WindowInfo& info : *listed )
        {
            result.push_back( WindowSummary{
                .id       = info.id,
                .wm_class = std::move( info.wm_class ),
                .title    = std::move( info.title ),
                .type     = std::move( info.type ),
                .pid      = info.pid,
                .bounds   = info.bounds,
            } );
        }
        return result;
    }

    grab::Result<WindowSummary>
    Screen::activate_window_by_class(
        const std::vector<std::string>& wm_class_candidates
    )
    {
        if( impl_ == nullptr )
        {
            return grab::fail( grab::ErrorCode::InternalFault, "Screen is not open" );
        }
        const std::vector<std::string> candidates =
            grab::screen::normalized_wm_class_candidates( wm_class_candidates );
        if( candidates.empty() )
        {
            return grab::fail( grab::ErrorCode::WindowNotFound,
                               "no WM_CLASS candidates were provided" );
        }

        auto listed = windows();
        if( !listed.has_value() )
        {
            return std::unexpected( std::move( listed.error() ) );
        }

        const auto match = std::ranges::find_if(
            *listed,
            [&candidates]( const WindowSummary& summary )
            {
                return grab::screen::wm_class_matches_any( summary.wm_class,
                                                           candidates );
            }
        );
        if( match == listed->end() )
        {
            return grab::fail( grab::ErrorCode::WindowNotFound,
                               "no window matched the requested WM_CLASS" );
        }

        auto active_display = connect_active_display( impl_->display_name() );
        if( !active_display.has_value() )
        {
            return std::unexpected( std::move( active_display.error() ) );
        }

        auto activated = raise_and_focus( active_display->connection.get(),
                                          active_display->root,
                                          static_cast<xcb_window_t>( match->id ) );
        if( !activated.has_value() )
        {
            return std::unexpected( std::move( activated.error() ) );
        }
        return std::move( *match );
    }

    grab::Result<Image>
    Screen::window_by_id( std::uint32_t window_id )
    {
        if( impl_ == nullptr )
        {
            return grab::fail( grab::ErrorCode::InternalFault, "Screen is not open" );
        }
        return image_of( impl_->route.capture_window( window_id ) );
    }

    grab::Result<void>
    Screen::activate_window( std::uint32_t window_id )
    {
        if( impl_ == nullptr )
        {
            return grab::fail( grab::ErrorCode::InternalFault, "Screen is not open" );
        }
        auto active_display = connect_active_display( impl_->display_name() );
        if( !active_display.has_value() )
        {
            return std::unexpected( std::move( active_display.error() ) );
        }
        return raise_and_focus( active_display->connection.get(),
                                active_display->root,
                                static_cast<xcb_window_t>( window_id ) );
    }

    grab::Result<grab::geometry::Rectangle>
    Screen::place_window( std::uint32_t                    window_id,
                          const grab::geometry::Rectangle& request,
                          std::chrono::milliseconds        timeout )
    {
        if( impl_ == nullptr )
        {
            return grab::fail( grab::ErrorCode::InternalFault, "Screen is not open" );
        }
        if( request.width == 0U || request.height == 0U )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "placement size must be non-zero" );
        }

        auto active_display = connect_active_display( impl_->display_name() );
        if( !active_display.has_value() )
        {
            return std::unexpected( std::move( active_display.error() ) );
        }
        xcb_connection_t* const connection = active_display->connection.get();
        const auto              window     = static_cast<xcb_window_t>( window_id );

        auto                    unmaximised =
            clear_maximised_state( connection, active_display->root, window );
        if( !unmaximised.has_value() )
        {
            return std::unexpected( std::move( unmaximised.error() ) );
        }

        auto requested =
            request_geometry( connection, active_display->root, window, request );
        if( !requested.has_value() )
        {
            return std::unexpected( std::move( requested.error() ) );
        }

        return await_placement( connection,
                                active_display->root,
                                window,
                                request,
                                timeout );
    }

}    // namespace grab
