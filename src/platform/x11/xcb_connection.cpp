#include "grab/result.hpp"
#include "platform/x11/xcb_connection.hpp"

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

        if( connection == nullptr || xcb_connection_has_error( connection ) != 0 )
        {
            if( connection != nullptr )
            {
                xcb_disconnect( connection );
            }
            return grab::fail( grab::ErrorCode::DisplayUnavailable,
                               "unable to open X display" );
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
