#include "grab/result.hpp"
#include "platform/x11/xcb_connection.hpp"
#include "platform/x11/xcb_reply.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <xcb/shm.h>
#include <xcb/xcb.h>
#include <xcb/xfixes.h>
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
                return grab::fail( grab::ErrorCode::display_unavailable,
                                   "X display setup is unavailable" );
            }

            const xcb_screen_iterator_t screens = xcb_setup_roots_iterator( setup );
            if( screens.rem == 0 || screens.data == nullptr )
            {
                return grab::fail( grab::ErrorCode::display_unavailable,
                                   "X display has no screens" );
            }
            return screens.data->root;
        }

        [[nodiscard]]
        bool
        query_shm( xcb_connection_t* connection ) noexcept
        {
            const xcb_query_extension_reply_t* extension =
                xcb_get_extension_data( connection, &xcb_shm_id );
            if( extension == nullptr || extension->present == 0 )
            {
                return false;
            }

            const xcb_shm_query_version_cookie_t cookie =
                xcb_shm_query_version( connection );
            auto reply = make_xcb_reply(
                xcb_shm_query_version_reply( connection, cookie, nullptr )
            );
            return reply != nullptr;
        }

        [[nodiscard]]
        bool
        query_xfixes( xcb_connection_t* connection ) noexcept
        {
            const xcb_query_extension_reply_t* extension =
                xcb_get_extension_data( connection, &xcb_xfixes_id );
            if( extension == nullptr || extension->present == 0 )
            {
                return false;
            }

            const xcb_xfixes_query_version_cookie_t cookie =
                xcb_xfixes_query_version( connection,
                                          XCB_XFIXES_MAJOR_VERSION,
                                          XCB_XFIXES_MINOR_VERSION );
            auto reply = make_xcb_reply(
                xcb_xfixes_query_version_reply( connection, cookie, nullptr )
            );
            return reply != nullptr;
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
                                  xcb_window_t      root,
                                  bool              has_shm,
                                  bool              has_xfixes ) noexcept :
        connection( connection ),
        root_window( root ),
        has_shm_extension( has_shm ),
        has_xfixes_extension( has_xfixes )
    {
    }

    XcbConnection::XcbConnection( XcbConnection&& other ) noexcept :
        connection( std::exchange( other.connection,
                                   nullptr ) ),
        root_window( std::exchange( other.root_window,
                                    XCB_NONE ) ),
        has_shm_extension( std::exchange( other.has_shm_extension,
                                          false ) ),
        has_xfixes_extension( std::exchange( other.has_xfixes_extension,
                                             false ) )
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
            connection           = std::exchange( other.connection, nullptr );
            root_window          = std::exchange( other.root_window, XCB_NONE );
            has_shm_extension    = std::exchange( other.has_shm_extension, false );
            has_xfixes_extension = std::exchange( other.has_xfixes_extension, false );
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
            return grab::fail( grab::ErrorCode::display_unavailable,
                               "unable to open X display" );
        }

        auto root = first_screen_root( connection );
        if( !root.has_value() )
        {
            xcb_disconnect( connection );
            return grab::fail( root.error().code, root.error().message );
        }

        const bool has_xfixes = query_xfixes( connection );
        return XcbConnection{
            connection,
            *root,
            query_shm( connection ),
            has_xfixes,
        };
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

    bool
    XcbConnection::has_shm() const noexcept
    {
        return has_shm_extension;
    }

    bool
    XcbConnection::has_xfixes() const noexcept
    {
        return has_xfixes_extension;
    }

}    // namespace grab::platform::x11
