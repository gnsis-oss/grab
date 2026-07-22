#include "grab/result.hpp"
#include "notify/notifier.hpp"

#include <chrono>
#include <cstdint>
#include <dbus/dbus.h>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace grab::notify
{
    namespace
    {

        constexpr dbus_uint32_t noReplacementId   = 0U;
        constexpr int           dbusTypeArray     = static_cast<int>( 'a' );
        constexpr int           dbusTypeInt32     = static_cast<int>( 'i' );
        constexpr int           dbusTypeString    = static_cast<int>( 's' );
        constexpr int           dbusTypeUint32    = static_cast<int>( 'u' );
        constexpr const char*   notificationsName = "org.freedesktop.Notifications";
        constexpr const char*   notificationsPath = "/org/freedesktop/Notifications";
        constexpr const char*   notificationsInterface = "org.freedesktop.Notifications";
        constexpr const char*   notifyMember           = "Notify";
        constexpr const char*   closeNotificationMember = "CloseNotification";
        constexpr const char*   stringArraySignature    = "s";
        constexpr const char*   hintsSignature          = "{sv}";
        constexpr auto          dbusTimeoutUseDefault =
            std::chrono::milliseconds{ DBUS_TIMEOUT_USE_DEFAULT };
        constexpr auto dbusTimeoutInfinite =
            std::chrono::milliseconds{ DBUS_TIMEOUT_INFINITE };

        [[nodiscard]]
        constexpr bool
        valid_dbus_timeout( std::chrono::milliseconds timeout ) noexcept
        {
            return timeout >= dbusTimeoutUseDefault && timeout <= dbusTimeoutInfinite;
        }

        [[nodiscard]]
        constexpr int
        dbus_timeout_milliseconds( std::chrono::milliseconds timeout ) noexcept
        {
            return static_cast<int>( timeout.count() );
        }

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

        using DbusConnectionPtr = std::unique_ptr<DBusConnection, DbusConnectionDeleter>;

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
        grab::Result<void>
        append_allocation_error( std::string_view call,
                                 std::string_view argument )
        {
            return grab::fail( grab::ErrorCode::InternalFault,
                               "D-Bus " +
                                   std::string{ call } +
                                   " " +
                                   std::string{ argument } +
                                   " argument allocation failed" );
        }

        [[nodiscard]]
        grab::Result<void>
        append_string( DBusMessageIter&   iterator,
                       const std::string& value,
                       std::string_view   argument )
        {
            const char* text = value.c_str();
            if( dbus_message_iter_append_basic( &iterator,
                                                dbusTypeString,
                                                static_cast<const void*>( &text ) ) ==
                0 )
            {
                return append_allocation_error( notifyMember, argument );
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        append_uint32( DBusMessageIter& iterator,
                       dbus_uint32_t    value,
                       std::string_view argument,
                       std::string_view call )
        {
            if( dbus_message_iter_append_basic( &iterator,
                                                dbusTypeUint32,
                                                static_cast<const void*>( &value ) ) ==
                0 )
            {
                return append_allocation_error( call, argument );
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        append_int32( DBusMessageIter& iterator,
                      dbus_int32_t     value,
                      std::string_view argument )
        {
            if( dbus_message_iter_append_basic( &iterator,
                                                dbusTypeInt32,
                                                static_cast<const void*>( &value ) ) ==
                0 )
            {
                return append_allocation_error( notifyMember, argument );
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        append_empty_array( DBusMessageIter& iterator,
                            const char*      signature,
                            std::string_view argument )
        {
            DBusMessageIter array_iterator{};
            if( dbus_message_iter_open_container( &iterator,
                                                  dbusTypeArray,
                                                  signature,
                                                  &array_iterator ) == 0 )
            {
                return append_allocation_error( notifyMember, argument );
            }

            if( dbus_message_iter_close_container( &iterator, &array_iterator ) == 0 )
            {
                return append_allocation_error( notifyMember, argument );
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        append_notify_arguments( DBusMessage*        message,
                                 const Notification& notification )
        {
            DBusMessageIter iterator{};
            dbus_message_iter_init_append( message, &iterator );

            auto appended = append_string( iterator, notification.app_name, "app_name" );
            if( !appended.has_value() )
            {
                return appended;
            }

            appended =
                append_uint32( iterator, noReplacementId, "replaces_id", notifyMember );
            if( !appended.has_value() )
            {
                return appended;
            }

            appended = append_string( iterator, notification.icon, "app_icon" );
            if( !appended.has_value() )
            {
                return appended;
            }

            appended = append_string( iterator, notification.summary, "summary" );
            if( !appended.has_value() )
            {
                return appended;
            }

            appended = append_string( iterator, notification.body, "body" );
            if( !appended.has_value() )
            {
                return appended;
            }

            appended = append_empty_array( iterator, stringArraySignature, "actions" );
            if( !appended.has_value() )
            {
                return appended;
            }

            appended = append_empty_array( iterator, hintsSignature, "hints" );
            if( !appended.has_value() )
            {
                return appended;
            }

            return append_int32( iterator, notification.timeout_ms, "expire_timeout" );
        }

        [[nodiscard]]
        grab::Result<std::uint32_t>
        parse_notify_id( DBusMessage* reply )
        {
            DBusMessageIter iterator{};
            if( dbus_message_iter_init( reply, &iterator ) ==
                0 ||
                dbus_message_iter_get_arg_type( &iterator ) != dbusTypeUint32 )
            {
                return grab::fail( grab::ErrorCode::ProtocolError,
                                   "D-Bus Notify response: missing notification id" );
            }

            dbus_uint32_t id = 0U;
            dbus_message_iter_get_basic( &iterator, static_cast<void*>( &id ) );
            return static_cast<std::uint32_t>( id );
        }

    }    // namespace

    struct Notifier::State
    {
            DbusConnectionPtr connection;
            NotifyOptions     options;
    };

    void
    DbusMessageDeleter::operator()( DBusMessage* message ) const noexcept
    {
        if( message != nullptr )
        {
            dbus_message_unref( message );
        }
    }

    grab::Result<DbusMessagePtr>
    build_notify_message( const Notification& notification )
    {
        DbusMessagePtr message{ dbus_message_new_method_call( notificationsName,
                                                              notificationsPath,
                                                              notificationsInterface,
                                                              notifyMember ) };
        if( message == nullptr )
        {
            return grab::fail( grab::ErrorCode::InternalFault,
                               "D-Bus Notify request allocation failed" );
        }

        auto appended = append_notify_arguments( message.get(), notification );
        if( !appended.has_value() )
        {
            return std::unexpected( std::move( appended.error() ) );
        }

        return std::move( message );
    }

    Notifier::Notifier( std::unique_ptr<State> state ) noexcept :
        state_( std::move( state ) )
    {
    }

    Notifier::~Notifier() = default;

    Notifier::Notifier( Notifier&& other ) noexcept :
        state_( std::move( other.state_ ) )
    {
    }

    Notifier&
    Notifier::operator=( Notifier&& other ) noexcept
    {
        if( this != &other )
        {
            state_ = std::move( other.state_ );
        }
        return *this;
    }

    grab::Result<Notifier>
    Notifier::open( NotifyOptions options )
    {
        if( !valid_dbus_timeout( options.timeout ) )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "notification D-Bus timeout is out of range" );
        }

        if( dbus_threads_init_default() == 0 )
        {
            return grab::fail( grab::ErrorCode::InternalFault,
                               "libdbus thread support initialization failed" );
        }

        DbusError         connection_error;
        DbusConnectionPtr connection{ dbus_bus_get_private( DBUS_BUS_SESSION,
                                                            &connection_error.value ) };
        if( connection == nullptr )
        {
            return std::unexpected(
                dbus_device_error( "notification session bus connection",
                                   connection_error )
            );
        }
        dbus_connection_set_exit_on_disconnect( connection.get(), 0 );

        auto state        = std::make_unique<State>();
        state->connection = std::move( connection );
        state->options    = options;
        return Notifier{ std::move( state ) };
    }

    grab::Result<std::uint32_t>
    Notifier::notify( const Notification& notification )
    {
        if( state_ == nullptr || state_->connection == nullptr )
        {
            return grab::fail( grab::ErrorCode::SessionClosed,
                               "notification D-Bus connection is closed" );
        }

        auto request = build_notify_message( notification );
        if( !request.has_value() )
        {
            return std::unexpected( std::move( request.error() ) );
        }

        DbusError            reply_error;
        const DbusMessagePtr reply{ dbus_connection_send_with_reply_and_block(
            state_->connection.get(),
            request->get(),
            dbus_timeout_milliseconds( state_->options.timeout ),
            &reply_error.value
        ) };
        if( reply == nullptr )
        {
            return std::unexpected( dbus_device_error( "D-Bus Notify", reply_error ) );
        }

        return parse_notify_id( reply.get() );
    }

    grab::Result<void>
    Notifier::close( std::uint32_t id )
    {
        if( state_ == nullptr || state_->connection == nullptr )
        {
            return grab::fail( grab::ErrorCode::SessionClosed,
                               "notification D-Bus connection is closed" );
        }

        const DbusMessagePtr request{
            dbus_message_new_method_call( notificationsName,
                                          notificationsPath,
                                          notificationsInterface,
                                          closeNotificationMember )
        };
        if( request == nullptr )
        {
            return grab::fail( grab::ErrorCode::InternalFault,
                               "D-Bus CloseNotification request allocation failed" );
        }

        DBusMessageIter iterator{};
        dbus_message_iter_init_append( request.get(), &iterator );
        auto appended = append_uint32( iterator,
                                       static_cast<dbus_uint32_t>( id ),
                                       "id",
                                       closeNotificationMember );
        if( !appended.has_value() )
        {
            return appended;
        }

        DbusError            reply_error;
        const DbusMessagePtr reply{ dbus_connection_send_with_reply_and_block(
            state_->connection.get(),
            request.get(),
            dbus_timeout_milliseconds( state_->options.timeout ),
            &reply_error.value
        ) };
        if( reply == nullptr )
        {
            return std::unexpected( dbus_device_error( "D-Bus CloseNotification",
                                                       reply_error ) );
        }

        return {};
    }

}    // namespace grab::notify
