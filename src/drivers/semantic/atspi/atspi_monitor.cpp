#include "drivers/semantic/atspi/atspi_monitor.hpp"
#include "grab/event.hpp"
#include "grab/event_descriptor.hpp"
#include "grab/result.hpp"
#include "kernel/events/event_bus.hpp"
#include "kernel/scheduling/reactor.hpp"
#include "kernel/support/ascii.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <dbus/dbus.h>
#include <expected>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <sys/epoll.h>    // IWYU pragma: keep
#include <utility>
#include <vector>

namespace grab::event
{
    namespace
    {

        constexpr int           dbusNoWait        = 0;
        constexpr int           dbusCallTimeoutMs = 1'000;
        constexpr int           invalidFd         = -1;
        constexpr std::uint64_t noToken           = 0U;
        constexpr std::uint32_t noEvents          = 0U;
        constexpr std::uint32_t readableEvents = static_cast<std::uint32_t>( EPOLLIN ) |
                                                 static_cast<std::uint32_t>( EPOLLERR ) |
                                                 static_cast<std::uint32_t>( EPOLLHUP );
        constexpr std::uint32_t writableEvents = static_cast<std::uint32_t>( EPOLLOUT ) |
                                                 static_cast<std::uint32_t>( EPOLLERR ) |
                                                 static_cast<std::uint32_t>( EPOLLHUP );
        constexpr const char*   a11yBusName    = "org.a11y.Bus";
        constexpr const char*   a11yBusPath    = "/org/a11y/bus";
        constexpr const char*   a11yBusInterface      = "org.a11y.Bus";
        constexpr const char*   getAddressMethod      = "GetAddress";
        constexpr const char*   registryName          = "org.a11y.atspi.Registry";
        constexpr const char*   registryPath          = "/org/a11y/atspi/registry";
        constexpr const char*   registryInterface     = "org.a11y.atspi.Registry";
        constexpr const char*   registerEventMethod   = "RegisterEvent";
        constexpr const char*   deregisterEventMethod = "DeregisterEvent";
        constexpr const char*   objectEventInterface  = "org.a11y.atspi.Event.Object";
        constexpr const char*   objectEventMatch =
            "type='signal',interface='org.a11y.atspi.Event.Object'";
        constexpr std::string_view stateChangedMember = "StateChanged";
        constexpr std::string_view textChangedMember  = "TextChanged";
        constexpr std::string_view actionMember       = "Action";
        constexpr std::string_view clickedMember      = "Clicked";
        constexpr std::string_view focusedDetail      = "focused";
        constexpr std::string_view pressedDetail      = "pressed";
        constexpr std::string_view checkedDetail      = "checked";
        constexpr std::string_view clickedDetail      = "clicked";
        constexpr std::string_view clickDetail        = "click";
        constexpr std::string_view activateDetail     = "activate";
        constexpr std::string_view actionDetail       = "action";
        constexpr std::string_view menuOpenedDetail   = "menu-opened";
        constexpr std::string_view menuClosedDetail   = "menu-closed";
        constexpr std::string_view showingDetail      = "showing";
        constexpr std::string_view visibleDetail      = "visible";
        constexpr std::string_view hiddenDetail       = "hidden";
        constexpr std::string_view collapsedDetail    = "collapsed";
        constexpr std::string_view menuRoleNeedle     = "menu";
        constexpr std::string_view buttonRoleNeedle   = "button";
        constexpr std::string_view pushRoleNeedle     = "push";
        constexpr std::string_view textChangedPrefix  = "text-changed";

        constexpr auto             registeredEvents = std::to_array<std::string_view>( {
            "object:state-changed:focused",
            "object:state-changed:pressed",
            "object:state-changed:checked",
            "object:state-changed:showing",
            "object:state-changed:visible",
            "object:text-changed",
        } );

        struct WatchRegistration
        {
                DBusWatch*    watch = nullptr;
                std::uint64_t token = noToken;
        };

        struct DbusError
        {
                DbusError()
                {
                    dbus_error_init( &value );
                }

                ~DbusError()
                {
                    if( dbus_error_is_set( &value ) != 0 )
                    {
                        dbus_error_free( &value );
                    }
                }

                DbusError( const DbusError& ) = delete;
                DbusError&
                operator=( const DbusError& ) = delete;
                DbusError( DbusError&& )      = delete;
                DbusError&
                operator=( DbusError&& ) = delete;

                [[nodiscard]]
                std::string
                message_or( std::string_view fallback ) const
                {
                    if( dbus_error_is_set( &value ) == 0 || value.message == nullptr )
                    {
                        return std::string{ fallback };
                    }
                    return value.message;
                }

                DBusError value{};
        };

        struct DbusConnectionDeleter
        {
                void
                operator()( DBusConnection* connection ) const noexcept
                {
                    if( connection == nullptr )
                    {
                        return;
                    }
                    dbus_connection_close( connection );
                    dbus_connection_unref( connection );
                }
        };

        struct DbusMessageDeleter
        {
                void
                operator()( DBusMessage* message ) const noexcept
                {
                    if( message != nullptr )
                    {
                        dbus_message_unref( message );
                    }
                }
        };

        using DbusConnection = std::unique_ptr<DBusConnection, DbusConnectionDeleter>;
        using DbusMessage    = std::unique_ptr<DBusMessage, DbusMessageDeleter>;

        [[nodiscard]]
        std::string
        dbus_text( const char* value )
        {
            if( value == nullptr )
            {
                return {};
            }
            return value;
        }

        [[nodiscard]]
        bool
        is_button_signal( const AtspiSignal& signal )
        {
            return grab::core::ascii_icontains( signal.role, buttonRoleNeedle ) ||
                   grab::core::ascii_icontains( signal.role, pushRoleNeedle ) ||
                   grab::core::ascii_icontains( signal.detail, clickedDetail ) ||
                   grab::core::ascii_icontains( signal.detail, clickDetail ) ||
                   grab::core::ascii_icontains( signal.detail, activateDetail ) ||
                   grab::core::ascii_icontains( signal.detail, actionDetail );
        }

        [[nodiscard]]
        bool
        is_menu_signal( const AtspiSignal& signal )
        {
            return grab::core::ascii_icontains( signal.role, menuRoleNeedle );
        }

        [[nodiscard]]
        grab::Event
        make_a11y_event( grab::EventKind    kind,
                         const AtspiSignal& signal,
                         double             timestamp )
        {
            return grab::Event{
                .timestamp = timestamp,
                .sequence  = 0U,
                .kind      = kind,
                .category  = grab::category_of( kind ),
                .payload   = grab::Payload{ grab::A11yEvent{
                    .app    = signal.app,
                    .role   = signal.role,
                    .name   = signal.name,
                    .detail = signal.detail,
                } },
            };
        }

        [[nodiscard]]
        std::optional<grab::EventKind>
        mapped_kind( const AtspiSignal& signal )
        {
            if( signal.interface != objectEventInterface )
            {
                return std::nullopt;
            }

            if( signal.member ==
                textChangedMember ||
                grab::core::ascii_istarts_with( signal.detail, textChangedPrefix ) )
            {
                return grab::EventKind::A11yTextChanged;
            }

            if( signal.detail ==
                menuOpenedDetail ||
                ( signal.member ==
                  stateChangedMember &&
                  is_menu_signal( signal ) &&
                  ( signal.detail ==
                    showingDetail ||
                    signal.detail == visibleDetail ) ) )
            {
                return grab::EventKind::A11yMenuOpened;
            }

            if( signal.detail ==
                menuClosedDetail ||
                ( signal.member ==
                  stateChangedMember &&
                  is_menu_signal( signal ) &&
                  ( signal.detail ==
                    hiddenDetail ||
                    signal.detail == collapsedDetail ) ) )
            {
                return grab::EventKind::A11yMenuClosed;
            }

            if( signal.member == stateChangedMember && signal.detail == focusedDetail )
            {
                return grab::EventKind::A11yFocusChanged;
            }

            if( ( signal.member ==
                  stateChangedMember &&
                  signal.detail ==
                  pressedDetail &&
                  is_button_signal( signal ) ) ||
                signal.member ==
                actionMember ||
                signal.member == clickedMember )
            {
                return grab::EventKind::A11yButtonClicked;
            }

            // Only details in the registered vocabulary decode; the broad
            // D-Bus match rule also delivers details other bus clients
            // registered ("defunct" fires for every accessible object an
            // app destroys — file dialogs and list scrolling emit hundreds).
            if( signal.member ==
                stateChangedMember &&
                ( signal.detail ==
                  checkedDetail ||
                  signal.detail ==
                  pressedDetail ||
                  signal.detail ==
                  showingDetail ||
                  signal.detail ==
                  visibleDetail ||
                  signal.detail ==
                  hiddenDetail ||
                  signal.detail == collapsedDetail ) )
            {
                return grab::EventKind::A11yStateChanged;
            }

            return std::nullopt;
        }

        [[nodiscard]]
        double
        current_timestamp()
        {
            const auto now = std::chrono::system_clock::now().time_since_epoch();
            return std::chrono::duration<double>{ now }.count();
        }

        [[nodiscard]]
        std::uint32_t
        epoll_events_for_watch( DBusWatch* watch )
        {
            std::uint32_t      events = static_cast<std::uint32_t>( EPOLLERR ) |
                                        static_cast<std::uint32_t>( EPOLLHUP );
            const unsigned int flags  = dbus_watch_get_flags( watch );
            if( ( flags & DBUS_WATCH_READABLE ) != 0U )
            {
                events |= static_cast<std::uint32_t>( EPOLLIN );
            }
            if( ( flags & DBUS_WATCH_WRITABLE ) != 0U )
            {
                events |= static_cast<std::uint32_t>( EPOLLOUT );
            }
            return events;
        }

        [[nodiscard]]
        unsigned int
        dbus_flags_for_epoll( std::uint32_t events )
        {
            unsigned int flags = 0U;
            if( ( events & static_cast<std::uint32_t>( EPOLLIN ) ) != 0U )
            {
                flags |= DBUS_WATCH_READABLE;
            }
            if( ( events & static_cast<std::uint32_t>( EPOLLOUT ) ) != 0U )
            {
                flags |= DBUS_WATCH_WRITABLE;
            }
            if( ( events & static_cast<std::uint32_t>( EPOLLERR ) ) != 0U )
            {
                flags |= DBUS_WATCH_ERROR;
            }
            if( ( events & static_cast<std::uint32_t>( EPOLLHUP ) ) != 0U )
            {
                flags |= DBUS_WATCH_HANGUP;
            }
            return flags;
        }

    }    // namespace

    AtspiEventRegistry::AtspiEventRegistry( Registrar registrar ) noexcept :
        registrar_( std::move( registrar ) )
    {
    }

    grab::Result<void>
    AtspiEventRegistry::acquire( std::string_view atspi_event )
    {
        const std::scoped_lock lock{ mutex_ };
        const auto             refcount =
            refcounts_.try_emplace( std::string{ atspi_event }, 0U ).first;
        ++refcount->second;
        if( refcount->second != 1U || !registrar_ )
        {
            return {};
        }

        auto registered = registrar_( atspi_event, true );
        if( !registered.has_value() )
        {
            refcounts_.erase( refcount );
        }
        return registered;
    }

    void
    AtspiEventRegistry::release( std::string_view atspi_event ) noexcept
    {
        try
        {
            const std::scoped_lock lock{ mutex_ };
            const auto             refcount = refcounts_.find( atspi_event );
            if( refcount == refcounts_.end() || refcount->second == 0U )
            {
                return;
            }

            --refcount->second;
            if( refcount->second != 0U )
            {
                return;
            }

            refcounts_.erase( refcount );
            if( registrar_ )
            {
                [[maybe_unused]]
                const auto deregistered = registrar_( atspi_event, false );
            }
        }
        catch( ... )
        {
            // Deregistration is best-effort and release() must not throw.
            return;
        }
    }

    std::size_t
    AtspiEventRegistry::demand( std::string_view atspi_event ) const noexcept
    {
        const std::scoped_lock lock{ mutex_ };
        const auto             refcount = refcounts_.find( atspi_event );
        if( refcount == refcounts_.end() )
        {
            return 0U;
        }
        return refcount->second;
    }

    struct AtspiMonitor::State : std::enable_shared_from_this<State>
    {
            std::mutex                          mutex;
            grab::core::Reactor*                reactor    = nullptr;
            DBusConnection*                     connection = nullptr;
            grab::EventBus*                     bus        = nullptr;
            std::unique_ptr<AtspiEventRegistry> registry;
            bool                                active = true;
            std::vector<WatchRegistration>      watches;
    };

    namespace
    {

        [[nodiscard]]
        std::vector<grab::Event>
        take_events_from_message( DBusMessage* message )
        {
            std::vector<grab::Event> events;
            AtspiSignal              signal{
                .interface = dbus_text( dbus_message_get_interface( message ) ),
                .member    = dbus_text( dbus_message_get_member( message ) ),
                .detail    = {},
                .app       = dbus_text( dbus_message_get_sender( message ) ),
                .role      = {},
                .name      = {},
            };

            if( signal.interface != objectEventInterface )
            {
                return events;
            }

            DBusMessageIter iterator{};
            if( dbus_message_iter_init( message, &iterator ) !=
                0 &&
                dbus_message_iter_get_arg_type( &iterator ) == DBUS_TYPE_STRING )
            {
                const char* detail = nullptr;
                dbus_message_iter_get_basic( &iterator, static_cast<void*>( &detail ) );
                signal.detail = dbus_text( detail );
            }

            auto decoded = decode_atspi_signal( signal, current_timestamp() );
            if( decoded.has_value() )
            {
                events.push_back( std::move( *decoded ) );
            }
            return events;
        }

        void
        append_pending_events( DBusConnection*           connection,
                               std::vector<grab::Event>& pending_events )
        {
            while( true )
            {
                const DbusMessage message{ dbus_connection_pop_message( connection ) };
                if( message == nullptr )
                {
                    break;
                }

                auto decoded = take_events_from_message( message.get() );
                pending_events.insert( pending_events.end(),
                                       std::make_move_iterator( decoded.begin() ),
                                       std::make_move_iterator( decoded.end() ) );
            }
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

        void
        remove_fd_noexcept( grab::core::Reactor* reactor,
                            std::uint64_t        token ) noexcept
        {
            if( reactor == nullptr || token == noToken )
            {
                return;
            }

            bool remove_failed = false;
            try
            {
                reactor->remove_fd( token );
            }
            catch( ... )
            {
                remove_failed = true;
            }
            static_cast<void>( remove_failed );
        }

        [[nodiscard]]
        grab::Error
        dbus_device_error( std::string_view step,
                           const DbusError& error )
        {
            return grab::Error{
                .code       = grab::ErrorCode::DeviceInaccessible,
                .message    = std::string{ step } +
                              ": " +
                              error.message_or( "D-Bus operation failed" ),
                .capability = {},
                .target     = {},
                .attempts   = {},
            };
        }

        [[nodiscard]]
        grab::Result<std::string>
        resolve_a11y_bus_address()
        {
            DbusError            connection_error;
            const DbusConnection session{
                dbus_bus_get_private( DBUS_BUS_SESSION, &connection_error.value )
            };
            if( session == nullptr )
            {
                return std::unexpected( dbus_device_error( "AT-SPI session bus lookup",
                                                           connection_error ) );
            }
            dbus_connection_set_exit_on_disconnect( session.get(), 0 );

            const DbusMessage request{
                dbus_message_new_method_call( a11yBusName,
                                              a11yBusPath,
                                              a11yBusInterface,
                                              getAddressMethod )
            };
            if( request == nullptr )
            {
                return grab::fail( grab::ErrorCode::InternalFault,
                                   "AT-SPI GetAddress request allocation failed" );
            }

            DbusError         reply_error;
            const DbusMessage reply{
                dbus_connection_send_with_reply_and_block( session.get(),
                                                           request.get(),
                                                           dbusCallTimeoutMs,
                                                           &reply_error.value )
            };
            if( reply == nullptr )
            {
                return std::unexpected( dbus_device_error( "AT-SPI bus address request",
                                                           reply_error ) );
            }

            DBusMessageIter iterator{};
            if( dbus_message_iter_init( reply.get(), &iterator ) ==
                0 ||
                dbus_message_iter_get_arg_type( &iterator ) != DBUS_TYPE_STRING )
            {
                return grab::fail( grab::ErrorCode::DeviceInaccessible,
                                   "AT-SPI bus address response: missing address" );
            }

            const char* address = nullptr;
            dbus_message_iter_get_basic( &iterator, static_cast<void*>( &address ) );
            if( address == nullptr )
            {
                return grab::fail( grab::ErrorCode::DeviceInaccessible,
                                   "AT-SPI bus address response: null address" );
            }

            return std::string{ address };
        }

        [[nodiscard]]
        grab::Result<DbusConnection>
        open_a11y_connection( const std::string& address )
        {
            DbusError      open_error;
            DbusConnection connection{
                dbus_connection_open_private( address.c_str(), &open_error.value )
            };
            if( connection == nullptr )
            {
                return std::unexpected( dbus_device_error( "AT-SPI bus connection",
                                                           open_error ) );
            }
            dbus_connection_set_exit_on_disconnect( connection.get(), 0 );

            DbusError register_error;
            if( dbus_bus_register( connection.get(), &register_error.value ) == 0 )
            {
                return std::unexpected( dbus_device_error( "AT-SPI bus registration",
                                                           register_error ) );
            }

            return connection;
        }

        [[nodiscard]]
        grab::Result<void>
        register_event( DBusConnection*  connection,
                        std::string_view event_name )
        {
            const DbusMessage request{
                dbus_message_new_method_call( registryName,
                                              registryPath,
                                              registryInterface,
                                              registerEventMethod )
            };
            if( request == nullptr )
            {
                return grab::fail( grab::ErrorCode::InternalFault,
                                   "AT-SPI RegisterEvent request allocation failed" );
            }

            DBusMessageIter iterator{};
            dbus_message_iter_init_append( request.get(), &iterator );
            const std::string event_string{ event_name };
            const char*       event_text = event_string.c_str();
            if( dbus_message_iter_append_basic(
                    &iterator,
                    DBUS_TYPE_STRING,
                    static_cast<const void*>( &event_text )
                ) == 0 )
            {
                return grab::fail( grab::ErrorCode::InternalFault,
                                   "AT-SPI RegisterEvent argument allocation failed" );
            }

            DbusError         reply_error;
            const DbusMessage reply{
                dbus_connection_send_with_reply_and_block( connection,
                                                           request.get(),
                                                           dbusCallTimeoutMs,
                                                           &reply_error.value )
            };
            if( reply == nullptr )
            {
                return std::unexpected( dbus_device_error( "AT-SPI RegisterEvent",
                                                           reply_error ) );
            }

            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        deregister_event( DBusConnection*  connection,
                          std::string_view event_name )
        {
            const DbusMessage request{
                dbus_message_new_method_call( registryName,
                                              registryPath,
                                              registryInterface,
                                              deregisterEventMethod )
            };
            if( request == nullptr )
            {
                return grab::fail( grab::ErrorCode::InternalFault,
                                   "AT-SPI DeregisterEvent request allocation failed" );
            }

            DBusMessageIter iterator{};
            dbus_message_iter_init_append( request.get(), &iterator );
            const std::string event_string{ event_name };
            const char*       event_text = event_string.c_str();
            if( dbus_message_iter_append_basic(
                    &iterator,
                    DBUS_TYPE_STRING,
                    static_cast<const void*>( &event_text )
                ) == 0 )
            {
                return grab::fail( grab::ErrorCode::InternalFault,
                                   "AT-SPI DeregisterEvent argument allocation failed" );
            }

            DbusError         reply_error;
            const DbusMessage reply{
                dbus_connection_send_with_reply_and_block( connection,
                                                           request.get(),
                                                           dbusCallTimeoutMs,
                                                           &reply_error.value )
            };
            if( reply == nullptr )
            {
                return std::unexpected( dbus_device_error( "AT-SPI DeregisterEvent",
                                                           reply_error ) );
            }

            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        add_object_event_match( DBusConnection* connection )
        {
            DbusError match_error;
            dbus_bus_add_match( connection, objectEventMatch, &match_error.value );
            if( dbus_error_is_set( &match_error.value ) != 0 )
            {
                return std::unexpected( dbus_device_error( "AT-SPI signal match",
                                                           match_error ) );
            }

            dbus_connection_flush( connection );
            return {};
        }

    }    // namespace

    std::optional<grab::Event>
    decode_atspi_signal( const AtspiSignal& signal,
                         double             timestamp )
    {
        const auto kind = mapped_kind( signal );
        if( !kind.has_value() )
        {
            return std::nullopt;
        }

        return make_a11y_event( *kind, signal, timestamp );
    }

    AtspiMonitor::AtspiMonitor( grab::core::Reactor&   reactor,
                                std::shared_ptr<State> state ) noexcept :
        reactor_( &reactor ),
        state_( std::move( state ) )
    {
    }

    AtspiMonitor::~AtspiMonitor()
    {
        stop();
    }

    AtspiMonitor::AtspiMonitor( AtspiMonitor&& other ) noexcept :
        reactor_( std::exchange( other.reactor_,
                                 nullptr ) ),
        state_( std::move( other.state_ ) )
    {
    }

    AtspiMonitor&
    AtspiMonitor::operator=( AtspiMonitor&& other ) noexcept
    {
        if( this != &other )
        {
            stop();
            reactor_ = std::exchange( other.reactor_, nullptr );
            state_   = std::move( other.state_ );
        }
        return *this;
    }

    std::uint64_t
    AtspiMonitor::register_watch_locked( State&     state,
                                         DBusWatch* watch )
    {
        if( !state.active ||
            state.reactor ==
            nullptr ||
            dbus_watch_get_enabled( watch ) == 0 )
        {
            return noToken;
        }

        const int fd = dbus_watch_get_unix_fd( watch );
        if( fd == invalidFd )
        {
            return noToken;
        }

        const auto shared_state = state.shared_from_this();
        return state.reactor->add_fd( fd,
                                      epoll_events_for_watch( watch ),
                                      [shared_state, watch]( std::uint32_t event_mask )
                                      {
                                          AtspiMonitor::handle_watch( shared_state,
                                                                      watch,
                                                                      event_mask );
                                      } );
    }

    bool
    AtspiMonitor::has_registered_watch_locked( const State& state,
                                               DBusWatch*   watch ) noexcept
    {
        return std::ranges::find_if( state.watches,
                                     [watch]( const WatchRegistration& candidate )
                                     {
                                         return candidate.watch ==
                                                watch &&
                                                candidate.token != noToken;
                                     } ) != state.watches.end();
    }

    dbus_bool_t
    AtspiMonitor::add_watch( DBusWatch* watch,
                             void*      data )
    {
        auto* const state = static_cast<State*>( data );
        if( state == nullptr || watch == nullptr )
        {
            return 0;
        }

        try
        {
            const std::scoped_lock lock( state->mutex );
            const std::uint64_t    token = register_watch_locked( *state, watch );
            state->watches.push_back( WatchRegistration{
                .watch = watch,
                .token = token,
            } );
            return 1;
        }
        catch( ... )
        {
            return 0;
        }
    }

    void
    AtspiMonitor::remove_watch( DBusWatch* watch,
                                void*      data ) noexcept
    {
        auto* const state = static_cast<State*>( data );
        if( state == nullptr || watch == nullptr )
        {
            return;
        }

        grab::core::Reactor* reactor = nullptr;
        std::uint64_t        token   = noToken;
        try
        {
            const std::scoped_lock lock( state->mutex );
            reactor = state->reactor;
            const auto registration =
                std::ranges::find_if( state->watches,
                                      [watch]( const WatchRegistration& candidate )
                                      {
                                          return candidate.watch == watch;
                                      } );
            if( registration == state->watches.end() )
            {
                return;
            }

            token = registration->token;
            state->watches.erase( registration );
        }
        catch( ... )
        {
            return;
        }

        remove_fd_noexcept( reactor, token );
    }

    void
    AtspiMonitor::toggle_watch( DBusWatch* watch,
                                void*      data ) noexcept
    {
        auto* const state = static_cast<State*>( data );
        if( state == nullptr || watch == nullptr )
        {
            return;
        }

        grab::core::Reactor* reactor      = nullptr;
        std::uint64_t        remove_token = noToken;
        try
        {
            const std::scoped_lock lock( state->mutex );
            const auto             registration =
                std::ranges::find_if( state->watches,
                                      [watch]( const WatchRegistration& candidate )
                                      {
                                          return candidate.watch == watch;
                                      } );
            if( registration == state->watches.end() )
            {
                const std::uint64_t token = register_watch_locked( *state, watch );
                state->watches.push_back( WatchRegistration{
                    .watch = watch,
                    .token = token,
                } );
                return;
            }

            if( dbus_watch_get_enabled( watch ) != 0 )
            {
                if( registration->token == noToken )
                {
                    registration->token = register_watch_locked( *state, watch );
                }
                return;
            }

            reactor             = state->reactor;
            remove_token        = registration->token;
            registration->token = noToken;
        }
        catch( ... )
        {
            return;
        }

        remove_fd_noexcept( reactor, remove_token );
    }

    grab::Result<AtspiMonitor>
    AtspiMonitor::start( grab::core::Reactor& reactor,
                         grab::EventBus&      bus )
    {
        if( dbus_threads_init_default() == 0 )
        {
            return grab::fail( grab::ErrorCode::InternalFault,
                               "libdbus thread support initialization failed" );
        }

        auto address = resolve_a11y_bus_address();
        if( !address.has_value() )
        {
            return std::unexpected( std::move( address.error() ) );
        }

        auto connection = open_a11y_connection( *address );
        if( !connection.has_value() )
        {
            return std::unexpected( std::move( connection.error() ) );
        }

        auto matched = add_object_event_match( connection->get() );
        if( !matched.has_value() )
        {
            return std::unexpected( std::move( matched.error() ) );
        }

        auto state        = std::make_shared<State>();
        state->reactor    = &reactor;
        state->connection = connection->get();
        state->bus        = &bus;

        if( dbus_connection_set_watch_functions( connection->get(),
                                                 &AtspiMonitor::add_watch,
                                                 &AtspiMonitor::remove_watch,
                                                 &AtspiMonitor::toggle_watch,
                                                 state.get(),
                                                 nullptr ) == 0 )
        {
            std::vector<std::uint64_t> tokens;
            {
                const std::scoped_lock lock( state->mutex );
                state->active  = false;
                state->bus     = nullptr;
                state->reactor = nullptr;
                for( const auto& watch : state->watches )
                {
                    if( watch.token != noToken )
                    {
                        tokens.push_back( watch.token );
                    }
                }
                state->watches.clear();
            }
            for( const std::uint64_t token : tokens )
            {
                remove_fd_noexcept( &reactor, token );
            }
            return grab::fail( grab::ErrorCode::DeviceInaccessible,
                               "AT-SPI D-Bus watch registration failed" );
        }

        DBusConnection* const registry_connection = connection->get();
        state->registry                           = std::make_unique<AtspiEventRegistry>(
            [registry_connection]( std::string_view event_name,
                                   bool             enable ) -> grab::Result<void>
            {
                if( enable )
                {
                    return register_event( registry_connection, event_name );
                }
                return deregister_event( registry_connection, event_name );
            }
        );
        state->connection = connection->release();
        return AtspiMonitor{ reactor, std::move( state ) };
    }

    grab::Result<void>
    AtspiMonitor::enable_events()
    {
        if( state_ == nullptr || state_->registry == nullptr )
        {
            return grab::fail( grab::ErrorCode::CapabilityUnavailable,
                               "AT-SPI event registry is unavailable" );
        }

        std::optional<grab::Error> first_error;
        for( const std::string_view event_name : registeredEvents )
        {
            auto acquired = state_->registry->acquire( event_name );
            if( !acquired.has_value() && !first_error.has_value() )
            {
                first_error.emplace( std::move( acquired.error() ) );
            }
        }

        if( first_error.has_value() )
        {
            return std::unexpected( std::move( *first_error ) );
        }
        return {};
    }

    void
    AtspiMonitor::disable_events() noexcept
    {
        if( state_ == nullptr || state_->registry == nullptr )
        {
            return;
        }

        for( const std::string_view event_name : registeredEvents )
        {
            state_->registry->release( event_name );
        }
    }

    void
    AtspiMonitor::handle_watch( const std::shared_ptr<State>& state,
                                DBusWatch*                    watch,
                                std::uint32_t                 events )
    {
        if( state ==
            nullptr ||
            watch ==
            nullptr ||
            ( events & ( readableEvents | writableEvents ) ) == noEvents )
        {
            return;
        }

        try
        {
            std::vector<grab::Event> pending_events;
            grab::EventBus*          bus = nullptr;
            {
                const std::scoped_lock lock( state->mutex );
                if( !state->active ||
                    state->connection ==
                    nullptr ||
                    state->bus ==
                    nullptr ||
                    !has_registered_watch_locked( *state, watch ) )
                {
                    return;
                }

                const unsigned int dbus_flags = dbus_flags_for_epoll( events );
                if( dbus_flags == 0U )
                {
                    return;
                }

                if( dbus_watch_handle( watch, dbus_flags ) == 0 )
                {
                    state->active = false;
                    return;
                }

                bus = state->bus;
                if( dbus_connection_read_write( state->connection, dbusNoWait ) == 0 )
                {
                    state->active = false;
                    return;
                }

                append_pending_events( state->connection, pending_events );
                if( dbus_connection_get_is_connected( state->connection ) == 0 )
                {
                    state->active = false;
                }
            }

            if( bus != nullptr )
            {
                publish_pending_events( *bus, pending_events );
            }
        }
        catch( ... )
        {
            // A reactor fd callback must never let an exception reach the
            // reactor: that would end run() and take down every other backend.
            // An unexpected dispatch failure deactivates this monitor (fail-safe)
            // rather than spinning on a message that keeps throwing.
            const std::scoped_lock lock( state->mutex );
            state->active = false;
        }
    }

    void
    AtspiMonitor::stop() noexcept
    {
        grab::core::Reactor* const reactor = std::exchange( reactor_, nullptr );
        auto                       state   = std::move( state_ );
        if( state == nullptr )
        {
            return;
        }

        DBusConnection* connection = nullptr;
        {
            std::vector<std::uint64_t> tokens;
            try
            {
                const std::scoped_lock lock( state->mutex );
                state->active  = false;
                state->bus     = nullptr;
                state->reactor = nullptr;
                connection     = std::exchange( state->connection, nullptr );
                for( const auto& watch : state->watches )
                {
                    if( watch.token != noToken )
                    {
                        tokens.push_back( watch.token );
                    }
                }
                state->watches.clear();
            }
            catch( ... )
            {
                return;
            }

            for( const std::uint64_t token : tokens )
            {
                remove_fd_noexcept( reactor, token );
            }
        }

        if( connection != nullptr )
        {
            dbus_connection_close( connection );
            dbus_connection_unref( connection );
        }
    }

}    // namespace grab::event
