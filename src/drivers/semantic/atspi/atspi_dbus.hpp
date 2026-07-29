#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

// Shared low-level D-Bus primitives for the AT-SPI driver: RAII wrappers over
// libdbus handles, the a11y-bus address discovery, and a private connection to
// the accessibility bus. Both the event monitor and the tree enumerator speak
// raw libdbus (the driver deliberately avoids libatspi/GObject so the vocabulary
// types stay toolkit-independent and testable without a live bus), so these
// helpers live here rather than duplicated per translation unit.

#include "grab/result.hpp"

#include <dbus/dbus.h>

#include <memory>
#include <string>
#include <string_view>

namespace grab::drivers::semantic::atspi::dbus
{

    // Blocking call budget for a single a11y-bus round trip. The a11y bus is a
    // local peer, so a second is generous; it bounds a wedged provider.
    inline constexpr int dbusCallTimeoutMs = 1'000;

    inline constexpr const char* a11yBusName      = "org.a11y.Bus";
    inline constexpr const char* a11yBusPath      = "/org/a11y/bus";
    inline constexpr const char* a11yBusInterface = "org.a11y.Bus";
    inline constexpr const char* getAddressMethod = "GetAddress";

    // Owns a DBusError: initialized on construction, freed on destruction. The
    // libdbus contract requires a paired init/free, and callers only ever read
    // the message, so the type is neither copyable nor movable.
    struct Error
    {
            Error()
            {
                dbus_error_init( &value );
            }

            ~Error()
            {
                if( dbus_error_is_set( &value ) != 0 )
                {
                    dbus_error_free( &value );
                }
            }

            Error( const Error& ) = delete;
            Error&
            operator=( const Error& ) = delete;
            Error( Error&& )          = delete;
            Error&
            operator=( Error&& ) = delete;

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

    struct ConnectionDeleter
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

    struct MessageDeleter
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

    using Connection = std::unique_ptr<DBusConnection, ConnectionDeleter>;
    using Message    = std::unique_ptr<DBusMessage, MessageDeleter>;

    // Wraps a libdbus failure as a grab DeviceInaccessible error, prefixing the
    // operation name so the log names the step that failed.
    [[nodiscard]]
    inline grab::Error
    device_error( std::string_view step,
                  const Error&     error )
    {
        return grab::Error{
            .code       = grab::ErrorCode::DeviceInaccessible,
            .message    = std::string{ step } + ": " +
                       error.message_or( "D-Bus operation failed" ),
            .capability = {},
            .target     = {},
            .attempts   = {},
        };
    }

    // Asks the session bus for the accessibility bus address
    // (org.a11y.Bus.GetAddress). A missing a11y bus surfaces as
    // DeviceInaccessible, which is the "accessibility is off" signal.
    [[nodiscard]]
    inline grab::Result<std::string>
    resolve_bus_address()
    {
        Error                connection_error;
        const Connection     session{
            dbus_bus_get_private( DBUS_BUS_SESSION, &connection_error.value )
        };
        if( session == nullptr )
        {
            return std::unexpected(
                device_error( "AT-SPI session bus lookup", connection_error ) );
        }
        dbus_connection_set_exit_on_disconnect( session.get(), 0 );

        const Message request{ dbus_message_new_method_call( a11yBusName,
                                                             a11yBusPath,
                                                             a11yBusInterface,
                                                             getAddressMethod ) };
        if( request == nullptr )
        {
            return grab::fail( grab::ErrorCode::InternalFault,
                               "AT-SPI GetAddress request allocation failed" );
        }

        Error         reply_error;
        const Message reply{
            dbus_connection_send_with_reply_and_block( session.get(),
                                                       request.get(),
                                                       dbusCallTimeoutMs,
                                                       &reply_error.value )
        };
        if( reply == nullptr )
        {
            return std::unexpected(
                device_error( "AT-SPI bus address request", reply_error ) );
        }

        DBusMessageIter iterator{};
        if( dbus_message_iter_init( reply.get(), &iterator ) == 0 ||
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

    // Opens and registers a private connection to the accessibility bus at the
    // given address. Private so the enumerator's traffic never shares the
    // session-bus connection the rest of the process uses.
    [[nodiscard]]
    inline grab::Result<Connection>
    open_connection( const std::string& address )
    {
        Error      open_error;
        Connection connection{
            dbus_connection_open_private( address.c_str(), &open_error.value )
        };
        if( connection == nullptr )
        {
            return std::unexpected(
                device_error( "AT-SPI bus connection", open_error ) );
        }
        dbus_connection_set_exit_on_disconnect( connection.get(), 0 );

        Error register_error;
        if( dbus_bus_register( connection.get(), &register_error.value ) == 0 )
        {
            return std::unexpected(
                device_error( "AT-SPI bus registration", register_error ) );
        }
        return connection;
    }

}    // namespace grab::drivers::semantic::atspi::dbus
