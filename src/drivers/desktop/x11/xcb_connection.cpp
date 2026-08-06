#include "drivers/desktop/x11/xcb_connection.hpp"
#include "grab/result.hpp"

#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <xcb/xcb.h>
#include <xcb/xproto.h>

namespace grab::platform::x11
{

    namespace
    {

        [[nodiscard]]
        grab::Result<xcb_window_t>
        first_screen_root( xcb_connection_t* connection )
        {
            const xcb_setup_t* setup = xcb_get_setup( connection );
            if( setup == nullptr )
            {
                return grab::fail( grab::ErrorCode::DisplayUnavailable,
                                   "X display setup is unavailable" );
            }

            const xcb_screen_iterator_t screens = xcb_setup_roots_iterator( setup );
            if( screens.rem == 0 || screens.data == nullptr )
            {
                return grab::fail( grab::ErrorCode::DisplayUnavailable,
                                   "X display has no screens" );
            }
            return screens.data->root;
        }

        // xcb_connection_has_error() returns WHICH failure it was, and that
        // code was being discarded. "unable to open X display" on a display
        // the caller can see running is not a diagnosis — it does not
        // distinguish a refused connection from a malformed display name from
        // a server that ran out of memory, and the three want different fixes.
        [[nodiscard]]
        std::string_view
        connection_error_reason( int code ) noexcept
        {
            switch( code )
            {
                case XCB_CONN_ERROR :
                    return "socket, pipe or stream error (the server may be out "
                           "of connection slots, or the caller out of file "
                           "descriptors)";
                case XCB_CONN_CLOSED_EXT_NOTSUPPORTED :
                    return "a required extension is not supported";
                case XCB_CONN_CLOSED_MEM_INSUFFICIENT :
                    return "out of memory";
                case XCB_CONN_CLOSED_REQ_LEN_EXCEED :
                    return "request length exceeded the server's maximum";
                case XCB_CONN_CLOSED_PARSE_ERR :
                    return "the display name could not be parsed";
                case XCB_CONN_CLOSED_INVALID_SCREEN :
                    return "the display has no screen with that number";
                case XCB_CONN_CLOSED_FDPASSING_FAILED :
                    return "file descriptor passing failed";
                default :
                    return "unrecognised connection error";
            }
        }

        [[nodiscard]]
        const char*
        display_name_or_default( const std::string& display ) noexcept
        {
            if( display.empty() )
            {
                return nullptr;
            }
            return display.c_str();
        }

    }    // namespace

    XcbConnection::XcbConnection( xcb_connection_t* connection,
                                  xcb_window_t      root ) noexcept :
        connection( connection ),
        root_window( root )
    {
    }

    XcbConnection::XcbConnection( XcbConnection&& other ) noexcept :
        connection( std::exchange( other.connection,
                                   nullptr ) ),
        root_window( std::exchange( other.root_window,
                                    XCB_NONE ) )
    {
    }

    XcbConnection&
    XcbConnection::operator=( XcbConnection&& other ) noexcept
    {
        if( this != &other )
        {
            if( connection != nullptr )
            {
                xcb_disconnect( connection );
            }
            connection  = std::exchange( other.connection, nullptr );
            root_window = std::exchange( other.root_window, XCB_NONE );
        }
        return *this;
    }

    XcbConnection::~XcbConnection()
    {
        if( connection != nullptr )
        {
            xcb_disconnect( connection );
        }
    }

    grab::Result<XcbConnection>
    XcbConnection::open( std::string_view display )
    {
        const std::string display_storage{ display };
        int               screen_index = 0;
        xcb_connection_t* connection =
            xcb_connect( display_name_or_default( display_storage ), &screen_index );
        ( void )screen_index;

        const int connection_error = connection == nullptr
                                       ? XCB_CONN_ERROR
                                       : xcb_connection_has_error( connection );
        if( connection_error != 0 )
        {
            if( connection != nullptr )
            {
                xcb_disconnect( connection );
            }
            // Name the display that was actually tried. An empty argument
            // means xcb read DISPLAY itself, and reporting an empty name there
            // sends the reader looking for a caller bug instead of at their
            // environment.
            const char* const environment_display = std::getenv( "DISPLAY" );
            const std::string attempted =
                display_storage.empty()
                    ? std::string{ "$DISPLAY=" } + ( environment_display == nullptr
                                                         ? "(unset)"
                                                         : environment_display )
                    : display_storage;
            return grab::fail(
                grab::ErrorCode::DisplayUnavailable,
                "unable to open X display '" +
                    attempted +
                    "': " +
                    std::string{ connection_error_reason( connection_error ) }
            );
        }

        auto root = first_screen_root( connection );
        if( !root.has_value() )
        {
            xcb_disconnect( connection );
            return grab::fail( root.error().code, root.error().message );
        }

        return XcbConnection{ connection, *root };
    }

    xcb_connection_t*
    XcbConnection::get() const noexcept
    {
        return connection;
    }

    xcb_window_t
    XcbConnection::root() const noexcept
    {
        return root_window;
    }

}    // namespace grab::platform::x11
