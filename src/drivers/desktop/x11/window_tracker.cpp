#include "drivers/desktop/x11/protocol.hpp"
#include "drivers/desktop/x11/window_tracker.hpp"
#include "grab/event.hpp"
#include "grab/pid.hpp"
#include "grab/result.hpp"
#include "kernel/events/event_bus.hpp"
#include "kernel/events/wall_clock.hpp"
#include "kernel/scheduling/reactor.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <sys/epoll.h>    // IWYU pragma: keep
#include <utility>
#include <vector>
#include <xcb/xcb.h>
#include <xcb/xproto.h>

namespace grab::drivers::desktop::x11
{
    namespace
    {

        constexpr int           invalidFd              = -1;
        constexpr int           xcbOk                  = 0;
        constexpr int           flushFailed            = 0;
        constexpr std::uint64_t noToken                = 0U;
        constexpr std::uint8_t  responseTypeMask       = 0X7FU;
        constexpr std::uint32_t propertyOffsetZero     = 0U;
        constexpr std::uint32_t maxPropertyUnits       = 256U * 1'024U;
        constexpr std::uint8_t  format8Bits            = 8U;
        constexpr std::uint8_t  format32Bits           = 32U;
        constexpr std::uint32_t noEvents               = 0U;
        constexpr double        unknownDurationSeconds = 0.0;
        constexpr std::uint32_t readableEvents = static_cast<std::uint32_t>( EPOLLIN ) |
                                                 static_cast<std::uint32_t>( EPOLLERR ) |
                                                 static_cast<std::uint32_t>( EPOLLHUP );
        constexpr std::uint32_t rootEventMask =
            static_cast<std::uint32_t>( XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY );
        constexpr std::uint32_t rootValueMask =
            static_cast<std::uint32_t>( XCB_CW_EVENT_MASK );

        template<typename T>
        using XcbOwned = std::unique_ptr<T, decltype( &std::free )>;

        using XcbConnection =
            std::unique_ptr<xcb_connection_t, decltype( &xcb_disconnect )>;

        struct Atoms
        {
                xcb_atom_t net_active_window = XCB_ATOM_NONE;
                xcb_atom_t net_wm_name       = XCB_ATOM_NONE;
                xcb_atom_t net_wm_pid        = XCB_ATOM_NONE;
                xcb_atom_t utf8_string       = XCB_ATOM_NONE;
        };

        struct TrackedWindow
        {
                xcb_window_t                          window = XCB_WINDOW_NONE;
                std::string                           app;
                grab::Pid                             pid;
                std::string                           title;
                std::chrono::steady_clock::time_point opened_at;
        };

        template<typename T>
        [[nodiscard]]
        XcbOwned<T>
        take_xcb_owned( T* pointer ) noexcept
        {
            return XcbOwned<T>{ pointer, &std::free };
        }

        [[nodiscard]]
        std::string
        window_id_string( xcb_window_t window )
        {
            return std::to_string( static_cast<std::uint32_t>( window ) );
        }

        [[nodiscard]]
        grab::Result<std::uint32_t>
        default_root( xcb_connection_t* connection,
                      int               screen_index )
        {
            xcb_screen_iterator_t iterator =
                xcb_setup_roots_iterator( xcb_get_setup( connection ) );
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

            return iterator.data->root;
        }

        [[nodiscard]]
        grab::Result<xcb_atom_t>
        intern_atom( xcb_connection_t* connection,
                     std::string_view  name )
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
                return grab::fail( grab::ErrorCode::ProtocolError,
                                   "XCB atom lookup failed for " + std::string{ name } );
            }

            return reply->atom;
        }

        [[nodiscard]]
        grab::Result<Atoms>
        intern_atoms( xcb_connection_t* connection )
        {
            auto net_active_window =
                intern_atom( connection,
                             grab::platform::x11::atom_name::netActiveWindow );
            if( !net_active_window.has_value() )
            {
                return std::unexpected( std::move( net_active_window.error() ) );
            }

            auto net_wm_name =
                intern_atom( connection, grab::platform::x11::atom_name::netWmName );
            if( !net_wm_name.has_value() )
            {
                return std::unexpected( std::move( net_wm_name.error() ) );
            }

            auto net_wm_pid =
                intern_atom( connection, grab::platform::x11::atom_name::netWmPid );
            if( !net_wm_pid.has_value() )
            {
                return std::unexpected( std::move( net_wm_pid.error() ) );
            }

            auto utf8_string =
                intern_atom( connection, grab::platform::x11::atom_name::utf8String );
            if( !utf8_string.has_value() )
            {
                return std::unexpected( std::move( utf8_string.error() ) );
            }

            return Atoms{
                .net_active_window = *net_active_window,
                .net_wm_name       = *net_wm_name,
                .net_wm_pid        = *net_wm_pid,
                .utf8_string       = *utf8_string,
            };
        }

        [[nodiscard]]
        bool
        stale_window_error( const xcb_generic_error_t& error ) noexcept
        {
            return error.error_code == XCB_WINDOW;
        }

        [[nodiscard]]
        std::optional<XcbOwned<xcb_get_property_reply_t>>
        read_property( xcb_connection_t* connection,
                       xcb_window_t      window,
                       xcb_atom_t        property,
                       xcb_atom_t        type )
        {
            if( property == XCB_ATOM_NONE )
            {
                return std::nullopt;
            }

            xcb_generic_error_t* raw_error = nullptr;
            auto                 reply     = take_xcb_owned(
                xcb_get_property_reply( connection,
                                        xcb_get_property( connection,
                                                          0U,
                                                          window,
                                                          property,
                                                          type,
                                                          propertyOffsetZero,
                                                          maxPropertyUnits ),
                                        &raw_error )
            );
            const auto error = take_xcb_owned( raw_error );
            if( error != nullptr )
            {
                if( stale_window_error( *error ) )
                {
                    return std::nullopt;
                }
                return std::nullopt;
            }
            if( reply == nullptr || reply->type == XCB_ATOM_NONE )
            {
                return std::nullopt;
            }
            return std::optional<XcbOwned<xcb_get_property_reply_t>>{
                std::move( reply )
            };
        }

        [[nodiscard]]
        std::optional<std::string>
        read_text_property( xcb_connection_t* connection,
                            xcb_window_t      window,
                            xcb_atom_t        property,
                            xcb_atom_t        type )
        {
            auto reply = read_property( connection, window, property, type );
            if( !reply.has_value() || ( *reply )->format != format8Bits )
            {
                return std::nullopt;
            }

            const int value_length = xcb_get_property_value_length( reply->get() );
            if( value_length <= 0 )
            {
                return std::string{};
            }

            const auto* const value =
                static_cast<const char*>( xcb_get_property_value( reply->get() ) );
            return std::string{ value, static_cast<std::size_t>( value_length ) };
        }

        [[nodiscard]]
        std::optional<std::uint32_t>
        read_u32_property( xcb_connection_t* connection,
                           xcb_window_t      window,
                           xcb_atom_t        property,
                           xcb_atom_t        type )
        {
            auto reply = read_property( connection, window, property, type );
            if( !reply.has_value() || ( *reply )->format != format32Bits )
            {
                return std::nullopt;
            }

            const int value_length = xcb_get_property_value_length( reply->get() );
            if( value_length <
                0 ||
                static_cast<std::size_t>( value_length ) < sizeof( std::uint32_t ) )
            {
                return std::nullopt;
            }

            const auto* const values = static_cast<const std::uint32_t*>(
                xcb_get_property_value( reply->get() )
            );
            return *values;
        }

        [[nodiscard]]
        std::string
        parse_wm_class_app( std::string_view value )
        {
            if( value.empty() )
            {
                return {};
            }

            const auto split = value.find( '\0' );
            if( split == std::string_view::npos )
            {
                return std::string{ value };
            }

            const auto class_begin = split + 1U;
            const auto class_end   = value.find( '\0', class_begin );
            if( class_begin < value.size() )
            {
                return std::string{ value.substr( class_begin,
                                                  class_end == std::string_view::npos
                                                      ? std::string_view::npos
                                                      : class_end - class_begin ) };
            }

            return std::string{ value.substr( 0U, split ) };
        }

        [[nodiscard]]
        std::string
        read_title( xcb_connection_t* connection,
                    xcb_window_t      window,
                    const Atoms&      atoms )
        {
            auto net_wm_name = read_text_property( connection,
                                                   window,
                                                   atoms.net_wm_name,
                                                   atoms.utf8_string );
            if( net_wm_name.has_value() )
            {
                return *net_wm_name;
            }

            auto wm_name = read_text_property( connection,
                                               window,
                                               XCB_ATOM_WM_NAME,
                                               XCB_ATOM_STRING );
            if( wm_name.has_value() )
            {
                return *wm_name;
            }

            return {};
        }

        [[nodiscard]]
        std::string
        read_app( xcb_connection_t* connection,
                  xcb_window_t      window )
        {
            auto wm_class = read_text_property( connection,
                                                window,
                                                XCB_ATOM_WM_CLASS,
                                                XCB_ATOM_STRING );
            if( wm_class.has_value() )
            {
                const std::string app = parse_wm_class_app( *wm_class );
                if( !app.empty() )
                {
                    return app;
                }
            }

            return window_id_string( window );
        }

        [[nodiscard]]
        grab::Pid
        read_pid( xcb_connection_t* connection,
                  xcb_window_t      window,
                  const Atoms&      atoms )
        {
            auto pid = read_u32_property( connection,
                                          window,
                                          atoms.net_wm_pid,
                                          XCB_ATOM_CARDINAL );
            if( !pid.has_value() )
            {
                return grab::Pid{};
            }
            return grab::Pid{ static_cast<std::int64_t>( *pid ) };
        }

        [[nodiscard]]
        TrackedWindow
        read_window_info( xcb_connection_t*                     connection,
                          xcb_window_t                          window,
                          const Atoms&                          atoms,
                          std::chrono::steady_clock::time_point opened_at )
        {
            return TrackedWindow{
                .window    = window,
                .app       = read_app( connection, window ),
                .pid       = read_pid( connection, window, atoms ),
                .title     = read_title( connection, window, atoms ),
                .opened_at = opened_at,
            };
        }

        [[nodiscard]]
        std::vector<TrackedWindow>::iterator
        find_tracked( std::vector<TrackedWindow>& windows,
                      xcb_window_t                window )
        {
            return std::ranges::find_if( windows,
                                         [window]( const TrackedWindow& candidate )
                                         {
                                             return candidate.window == window;
                                         } );
        }

        [[nodiscard]]
        TrackedWindow&
        ensure_tracked( xcb_connection_t*           connection,
                        xcb_window_t                window,
                        const Atoms&                atoms,
                        std::vector<TrackedWindow>& windows )
        {
            const auto existing = find_tracked( windows, window );
            if( existing != windows.end() )
            {
                existing->app   = read_app( connection, window );
                existing->pid   = read_pid( connection, window, atoms );
                existing->title = read_title( connection, window, atoms );
                return *existing;
            }

            windows.push_back( read_window_info( connection,
                                                 window,
                                                 atoms,
                                                 std::chrono::steady_clock::now() ) );
            return windows.back();
        }

        [[nodiscard]]
        grab::WindowChange
        make_window_change( const TrackedWindow& info )
        {
            return grab::WindowChange{
                .app        = info.app,
                .pid        = info.pid,
                .title      = info.title,
                .prev_title = {},
                .duration_s = 0.0,
            };
        }

        [[nodiscard]]
        grab::Event
        make_window_event( grab::EventKind    kind,
                           grab::WindowChange payload )
        {
            return grab::Event{
                .timestamp = grab::kernel::now_timestamp_s(),
                .sequence  = 0U,
                .kind      = kind,
                .category  = grab::EventCategory::Window,
                .payload   = grab::Payload{ std::move( payload ) },
            };
        }

        [[nodiscard]]
        grab::Event
        make_active_child_event( xcb_window_t previous_active,
                                 xcb_window_t current_active )
        {
            return grab::Event{
                .timestamp = grab::kernel::now_timestamp_s(),
                .sequence  = 0U,
                .kind      = grab::EventKind::ActiveChildChanged,
                .category  = grab::EventCategory::Window,
                .payload   = grab::Payload{ grab::GraphChange{
                    .node            = static_cast<std::uint64_t>( current_active ),
                    .related         = 0U,
                    .relation        = 0U,
                    .previous_active = static_cast<std::uint64_t>( previous_active ),
                } },
            };
        }

        [[nodiscard]]
        grab::Event
        make_empty_focus_event()
        {
            return make_window_event( grab::EventKind::WindowFocusChanged,
                                      grab::WindowChange{} );
        }

        [[nodiscard]]
        double
        duration_seconds( std::chrono::steady_clock::time_point opened_at,
                          std::chrono::steady_clock::time_point closed_at )
        {
            return std::chrono::duration<double>( closed_at - opened_at ).count();
        }

        void
        publish_pending_events( grab::EventBus&           bus,
                                std::vector<grab::Event>& events ) noexcept
        {
            for( auto& event : events )
            {
                bus.publish( std::move( event ) );
            }
        }

        [[nodiscard]]
        grab::Result<void>
        check_request( xcb_connection_t* connection,
                       xcb_void_cookie_t cookie,
                       std::string_view  operation )
        {
            const auto error = take_xcb_owned( xcb_request_check( connection, cookie ) );
            if( error != nullptr )
            {
                return grab::fail( grab::ErrorCode::ProtocolError,
                                   std::string{ operation } +
                                       " failed with X error " +
                                       std::to_string( error->error_code ) );
            }

            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        select_root_events( xcb_connection_t* connection,
                            xcb_window_t      root )
        {
            const std::uint32_t event_mask = rootEventMask;
            auto                selected =
                check_request( connection,
                               xcb_change_window_attributes_checked( connection,
                                                                     root,
                                                                     rootValueMask,
                                                                     &event_mask ),
                               "XCB root event selection" );
            if( !selected.has_value() )
            {
                return selected;
            }

            if( xcb_flush( connection ) <=
                flushFailed ||
                xcb_connection_has_error( connection ) != xcbOk )
            {
                return grab::fail( grab::ErrorCode::DeviceInaccessible,
                                   "XCB root event selection flush failed" );
            }

            return {};
        }

        [[nodiscard]]
        xcb_window_t
        active_window( xcb_connection_t* connection,
                       xcb_window_t      root,
                       const Atoms&      atoms )
        {
            auto active = read_u32_property( connection,
                                             root,
                                             atoms.net_active_window,
                                             XCB_ATOM_WINDOW );
            if( !active.has_value() )
            {
                return XCB_WINDOW_NONE;
            }
            return static_cast<xcb_window_t>( *active );
        }

        void
        handle_create( xcb_connection_t*           connection,
                       const Atoms&                atoms,
                       xcb_window_t                window,
                       std::vector<TrackedWindow>& tracked,
                       std::vector<grab::Event>&   pending_events )
        {
            const auto opened_at = std::chrono::steady_clock::now();
            auto       info = read_window_info( connection, window, atoms, opened_at );
            const auto existing = find_tracked( tracked, window );
            if( existing == tracked.end() )
            {
                tracked.push_back( info );
            }
            else
            {
                *existing = info;
            }

            pending_events.push_back( make_window_event( grab::EventKind::WindowCreated,
                                                         make_window_change( info ) ) );
        }

        void
        handle_destroy( xcb_window_t                window,
                        std::vector<TrackedWindow>& tracked,
                        std::vector<grab::Event>&   pending_events )
        {
            grab::WindowChange payload{
                .app        = window_id_string( window ),
                .pid        = {},
                .title      = {},
                .prev_title = {},
                .duration_s = unknownDurationSeconds,
            };

            const auto existing = find_tracked( tracked, window );
            if( existing != tracked.end() )
            {
                payload.app   = existing->app;
                payload.pid   = existing->pid;
                payload.title = existing->title;
                payload.duration_s =
                    duration_seconds( existing->opened_at,
                                      std::chrono::steady_clock::now() );
                tracked.erase( existing );
            }

            pending_events.push_back( make_window_event( grab::EventKind::WindowClosed,
                                                         std::move( payload ) ) );
        }

        void
        append_decoded_event( const xcb_generic_event_t&  raw_event,
                              xcb_connection_t*           connection,
                              const Atoms&                atoms,
                              std::vector<TrackedWindow>& tracked,
                              std::vector<grab::Event>&   pending_events )
        {
            const auto response_type =
                static_cast<std::uint8_t>( raw_event.response_type & responseTypeMask );

            const void* const event_storage = &raw_event;
            switch( response_type )
            {
                case XCB_CREATE_NOTIFY :
                    {
                        const auto* const create =
                            static_cast<const xcb_create_notify_event_t*>(
                                event_storage
                            );
                        handle_create( connection,
                                       atoms,
                                       create->window,
                                       tracked,
                                       pending_events );
                        break;
                    }
                case XCB_DESTROY_NOTIFY :
                    {
                        const auto* const destroy =
                            static_cast<const xcb_destroy_notify_event_t*>(
                                event_storage
                            );
                        handle_destroy( destroy->window, tracked, pending_events );
                        break;
                    }
                default :
                    break;
            }
        }

    }    // namespace

    struct WindowTracker::State
    {
            std::mutex                 callback_mutex;
            std::mutex                 mutex;
            xcb_connection_t*          connection = nullptr;
            grab::EventBus*            bus        = nullptr;
            grab::core::Reactor*       reactor    = nullptr;
            xcb_window_t               root       = XCB_WINDOW_NONE;
            Atoms                      atoms;
            bool                       active            = true;
            bool                       has_polled_active = false;
            xcb_window_t               active_window     = XCB_WINDOW_NONE;
            std::chrono::milliseconds  poll_interval{};
            std::vector<TrackedWindow> tracked;
    };

    WindowTracker::WindowTracker( grab::core::Reactor&   reactor,
                                  std::uint64_t          fd_token,
                                  std::uint64_t          timer_token,
                                  std::shared_ptr<State> state ) noexcept :
        reactor_( &reactor ),
        fd_token_( fd_token ),
        timer_token_( timer_token ),
        state_( std::move( state ) )
    {
    }

    WindowTracker::~WindowTracker()
    {
        stop();
    }

    WindowTracker::WindowTracker( WindowTracker&& other ) noexcept :
        reactor_( std::exchange( other.reactor_,
                                 nullptr ) ),
        fd_token_( std::exchange( other.fd_token_,
                                  noToken ) ),
        timer_token_( std::exchange( other.timer_token_,
                                     noToken ) ),
        state_( std::move( other.state_ ) )
    {
    }

    WindowTracker&
    WindowTracker::operator=( WindowTracker&& other ) noexcept
    {
        if( this != &other )
        {
            stop();
            reactor_     = std::exchange( other.reactor_, nullptr );
            fd_token_    = std::exchange( other.fd_token_, noToken );
            timer_token_ = std::exchange( other.timer_token_, noToken );
            state_       = std::move( other.state_ );
        }
        return *this;
    }

    grab::Result<WindowTracker>
    WindowTracker::start( const char*               display,
                          grab::core::Reactor&      reactor,
                          grab::EventBus&           bus,
                          std::chrono::milliseconds poll_interval )
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

        auto root = default_root( connection.get(), screen_index );
        if( !root.has_value() )
        {
            return std::unexpected( std::move( root.error() ) );
        }

        auto atoms = intern_atoms( connection.get() );
        if( !atoms.has_value() )
        {
            return std::unexpected( std::move( atoms.error() ) );
        }

        auto selected = select_root_events( connection.get(), *root );
        if( !selected.has_value() )
        {
            return std::unexpected( std::move( selected.error() ) );
        }

        const int fd = xcb_get_file_descriptor( connection.get() );
        if( fd == invalidFd )
        {
            return grab::fail( grab::ErrorCode::DeviceInaccessible,
                               "XCB connection file descriptor is unavailable" );
        }

        auto state           = std::make_shared<State>();
        state->connection    = connection.release();
        state->bus           = &bus;
        state->reactor       = &reactor;
        state->root          = *root;
        state->atoms         = *atoms;
        state->poll_interval = poll_interval;

        const auto fd_token =
            reactor.add_fd( fd,
                            static_cast<std::uint32_t>( EPOLLIN ),
                            [state]( std::uint32_t event_mask )
                            {
                                WindowTracker::handle_fd( state, event_mask );
                            } );
        const auto timer_token =
            reactor.add_timer( poll_interval,
                               [state]
                               {
                                   WindowTracker::handle_poll( state );
                               } );

        return WindowTracker{ reactor, fd_token, timer_token, std::move( state ) };
    }

    void
    WindowTracker::handle_fd( const std::shared_ptr<State>& state,
                              std::uint32_t                 events )
    {
        if( ( events & readableEvents ) == noEvents )
        {
            return;
        }

        std::vector<grab::Event> pending_events;
        grab::EventBus*          bus = nullptr;
        const std::scoped_lock   callback_lock( state->callback_mutex );
        {
            const std::scoped_lock lock( state->mutex );
            if( !state->active || state->connection == nullptr || state->bus == nullptr )
            {
                return;
            }

            bus = state->bus;
            while( true )
            {
                const auto event =
                    take_xcb_owned( xcb_poll_for_event( state->connection ) );
                if( event == nullptr )
                {
                    break;
                }

                append_decoded_event( *event,
                                      state->connection,
                                      state->atoms,
                                      state->tracked,
                                      pending_events );
            }

            if( xcb_connection_has_error( state->connection ) != xcbOk )
            {
                state->active = false;
            }
        }

        if( bus != nullptr )
        {
            publish_pending_events( *bus, pending_events );
        }
    }

    void
    WindowTracker::handle_poll( const std::shared_ptr<State>& state )
    {
        std::vector<grab::Event> pending_events;
        grab::EventBus*          bus           = nullptr;
        grab::core::Reactor*     reactor       = nullptr;
        auto                     poll_interval = std::chrono::milliseconds{};

        const std::scoped_lock   callback_lock( state->callback_mutex );
        {
            const std::scoped_lock lock( state->mutex );
            if( !state->active || state->connection == nullptr || state->bus == nullptr )
            {
                return;
            }

            bus           = state->bus;
            reactor       = state->reactor;
            poll_interval = state->poll_interval;

            const xcb_window_t current_active =
                active_window( state->connection, state->root, state->atoms );
            const bool had_polled_before = state->has_polled_active;
            const bool active_changed =
                !had_polled_before || current_active != state->active_window;
            state->has_polled_active = true;

            if( active_changed )
            {
                const xcb_window_t prev_active = state->active_window;
                state->active_window           = current_active;
                if( current_active == XCB_WINDOW_NONE )
                {
                    pending_events.push_back( make_empty_focus_event() );
                }
                else
                {
                    const TrackedWindow& info = ensure_tracked( state->connection,
                                                                current_active,
                                                                state->atoms,
                                                                state->tracked );
                    pending_events.push_back(
                        make_window_event( grab::EventKind::WindowFocusChanged,
                                           make_window_change( info ) )
                    );
                }
                if( had_polled_before )
                {
                    pending_events.push_back(
                        make_active_child_event( prev_active, current_active )
                    );
                }
            }
            else if( current_active != XCB_WINDOW_NONE )
            {
                const auto existing = find_tracked( state->tracked, current_active );
                if( existing == state->tracked.end() )
                {
                    state->tracked.push_back(
                        read_window_info( state->connection,
                                          current_active,
                                          state->atoms,
                                          std::chrono::steady_clock::now() )
                    );
                }
                else
                {
                    const std::string old_title = existing->title;
                    const std::string new_title =
                        read_title( state->connection, current_active, state->atoms );
                    if( !new_title.empty() && new_title != old_title )
                    {
                        existing->title    = new_title;
                        auto payload       = make_window_change( *existing );
                        payload.prev_title = old_title;
                        pending_events.push_back(
                            make_window_event( grab::EventKind::WindowTitleChanged,
                                               std::move( payload ) )
                        );
                    }
                }
            }

            if( reactor != nullptr && state->active )
            {
                static_cast<void>(
                    reactor->add_timer( poll_interval,
                                        [state]
                                        {
                                            WindowTracker::handle_poll( state );
                                        } )
                );
            }
        }

        if( bus != nullptr )
        {
            publish_pending_events( *bus, pending_events );
        }
    }

    void
    WindowTracker::stop() noexcept
    {
        grab::core::Reactor* const reactor  = std::exchange( reactor_, nullptr );
        const std::uint64_t        fd_token = std::exchange( fd_token_, noToken );
        static_cast<void>( std::exchange( timer_token_, noToken ) );
        auto state = std::move( state_ );

        if( reactor != nullptr && fd_token != noToken )
        {
            bool remove_failed = false;
            try
            {
                reactor->remove_fd( fd_token );
            }
            catch( ... )
            {
                remove_failed = true;
            }
            static_cast<void>( remove_failed );
        }

        if( state == nullptr )
        {
            return;
        }

        xcb_connection_t* connection = nullptr;
        try
        {
            const std::scoped_lock callback_lock( state->callback_mutex );
            const std::scoped_lock lock( state->mutex );
            state->active  = false;
            state->bus     = nullptr;
            state->reactor = nullptr;
            connection     = std::exchange( state->connection, nullptr );
        }
        catch( ... )
        {
            return;
        }

        if( connection != nullptr )
        {
            xcb_disconnect( connection );
        }
    }

}    // namespace grab::drivers::desktop::x11
