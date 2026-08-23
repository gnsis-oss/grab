#include "grab/result.hpp"
#include "session/display_probe.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <unistd.h>
#include <utility>
#include <xcb/xcb.h>
#include <xcb/xproto.h>

namespace grab::session
{
    namespace
    {

        constexpr int           xcbOk               = 0;
        constexpr int           accessSuccess       = 0;
        constexpr int           pathExistsMode      = F_OK;
        constexpr char          displayPrefix       = ':';
        constexpr const char*   displayLockPrefix   = "/tmp/.X";
        constexpr const char*   displayLockSuffix   = "-lock";
        constexpr const char*   displaySocketPrefix = "/tmp/.X11-unix/X";
        constexpr const char*   supportingWmCheck   = "_NET_SUPPORTING_WM_CHECK";
        constexpr const char*   atSpiBusProperty    = "AT_SPI_BUS";
        constexpr const char*   compositorPrefix    = "_NET_WM_CM_S";
        constexpr const char*   managerPrefix       = "WM_S";
        constexpr std::uint8_t  onlyIfExists        = 1U;
        constexpr std::uint8_t  keepProperty        = 0U;
        constexpr std::uint32_t noPropertyOffset    = 0U;
        // A window id is one 32-bit value; a bus address is a short string.
        constexpr std::uint32_t windowPropertyWords  = 1U;
        constexpr std::uint32_t addressPropertyWords = 256U;

        template<typename T>
        using XcbReply = std::unique_ptr<T, void ( * )( void* )>;

        template<typename T>
        [[nodiscard]]
        XcbReply<T>
        own_reply( T* reply ) noexcept
        {
            return XcbReply<T>{ reply, &std::free };
        }

        [[nodiscard]]
        bool
        path_exists( const std::string& path ) noexcept
        {
            if( access( path.c_str(), pathExistsMode ) == accessSuccess )
            {
                return true;
            }
            // Anything other than "it is not there" is an answer we cannot
            // trust, so treat it as occupied rather than claim the number.
            return errno != ENOENT && errno != ENOTDIR;
        }

        [[nodiscard]]
        bool
        display_has_existing_artifact( int display_number )
        {
            const std::string number = std::to_string( display_number );
            return path_exists(
                       std::string{ displayLockPrefix } + number + displayLockSuffix
                   ) ||
                   path_exists( std::string{ displaySocketPrefix } + number );
        }

    }    // namespace

    std::string
    display_name_for( int display_number )
    {
        return std::string( 1U, displayPrefix ) + std::to_string( display_number );
    }

    bool
    display_connectable( const std::string& display )
    {
        int                     screen_index = 0;
        xcb_connection_t* const connection =
            xcb_connect( display.c_str(), &screen_index );
        if( connection == nullptr )
        {
            return false;
        }
        const bool usable = xcb_connection_has_error( connection ) == xcbOk;
        xcb_disconnect( connection );
        return usable;
    }

    grab::Result<std::string>
    find_free_display()
    {
        for( int display_number = firstProvisionableDisplay;
             display_number <= lastProvisionableDisplay;
             ++display_number )
        {
            if( display_has_existing_artifact( display_number ) )
            {
                continue;
            }

            std::string display = display_name_for( display_number );
            if( !display_connectable( display ) )
            {
                return display;
            }
        }

        return grab::fail( grab::ErrorCode::DeviceInaccessible,
                           "No free X display found in " +
                               display_name_for( firstProvisionableDisplay ) +
                               ".." +
                               display_name_for( lastProvisionableDisplay ) );
    }

    DisplayProbe::DisplayProbe( xcb_connection_t* connection,
                                xcb_window_t      root,
                                int               screen_index ) noexcept :
        connection_( connection ),
        root_( root ),
        screen_index_( screen_index )
    {
    }

    DisplayProbe::~DisplayProbe()
    {
        close();
    }

    DisplayProbe::DisplayProbe( DisplayProbe&& other ) noexcept :
        connection_( std::exchange( other.connection_,
                                    nullptr ) ),
        root_( std::exchange( other.root_,
                              XCB_NONE ) ),
        screen_index_( std::exchange( other.screen_index_,
                                      0 ) )
    {
    }

    DisplayProbe&
    DisplayProbe::operator=( DisplayProbe&& other ) noexcept
    {
        if( this != &other )
        {
            close();
            connection_   = std::exchange( other.connection_, nullptr );
            root_         = std::exchange( other.root_, XCB_NONE );
            screen_index_ = std::exchange( other.screen_index_, 0 );
        }
        return *this;
    }

    void
    DisplayProbe::close() noexcept
    {
        if( connection_ != nullptr )
        {
            xcb_disconnect( connection_ );
            connection_ = nullptr;
        }
        root_ = XCB_NONE;
    }

    grab::Result<DisplayProbe>
    DisplayProbe::open( const std::string& display )
    {
        int                     screen_index = 0;
        xcb_connection_t* const connection =
            xcb_connect( display.c_str(), &screen_index );
        if( connection == nullptr || xcb_connection_has_error( connection ) != xcbOk )
        {
            if( connection != nullptr )
            {
                xcb_disconnect( connection );
            }
            return grab::fail( grab::ErrorCode::DisplayUnavailable,
                               "cannot connect to X display '" + display + "'" );
        }

        const xcb_setup_t* const setup = xcb_get_setup( connection );
        if( setup == nullptr )
        {
            xcb_disconnect( connection );
            return grab::fail( grab::ErrorCode::DisplayUnavailable,
                               "X display '" + display + "' reported no setup" );
        }

        xcb_screen_iterator_t screens = xcb_setup_roots_iterator( setup );
        for( int index = 0; index < screen_index && screens.rem > 1; ++index )
        {
            xcb_screen_next( &screens );
        }
        if( screens.rem == 0 || screens.data == nullptr )
        {
            xcb_disconnect( connection );
            return grab::fail( grab::ErrorCode::DisplayUnavailable,
                               "X display '" + display + "' has no screens" );
        }

        return DisplayProbe{ connection, screens.data->root, screen_index };
    }

    bool
    DisplayProbe::connected() const noexcept
    {
        return connection_ !=
               nullptr &&
               xcb_connection_has_error( connection_ ) == xcbOk;
    }

    xcb_atom_t
    DisplayProbe::existing_atom( const std::string& name ) const
    {
        if( !connected() )
        {
            return XCB_ATOM_NONE;
        }
        xcb_generic_error_t* raw_error = nullptr;
        const auto           reply     = own_reply( xcb_intern_atom_reply(
            connection_,
            xcb_intern_atom( connection_,
                             onlyIfExists,
                             static_cast<std::uint16_t>( name.size() ),
                             name.data() ),
            &raw_error
        ) );
        const auto           error     = own_reply( raw_error );
        if( error != nullptr || reply == nullptr )
        {
            return XCB_ATOM_NONE;
        }
        return reply->atom;
    }

    bool
    DisplayProbe::selection_owned( const std::string& name ) const
    {
        const xcb_atom_t selection = existing_atom( name );
        if( selection == XCB_ATOM_NONE )
        {
            return false;
        }
        xcb_generic_error_t* raw_error = nullptr;
        const auto           reply     = own_reply( xcb_get_selection_owner_reply(
            connection_,
            xcb_get_selection_owner( connection_, selection ),
            &raw_error
        ) );
        const auto           error     = own_reply( raw_error );
        if( error != nullptr || reply == nullptr )
        {
            return false;
        }
        return reply->owner != XCB_WINDOW_NONE;
    }

    std::string
    DisplayProbe::compositor_selection_name() const
    {
        return std::string{ compositorPrefix } + std::to_string( screen_index_ );
    }

    bool
    DisplayProbe::window_manager_present() const
    {
        const xcb_atom_t check = existing_atom( supportingWmCheck );
        if( check != XCB_ATOM_NONE )
        {
            xcb_generic_error_t* raw_error = nullptr;
            // NOLINTBEGIN(readability-suspicious-call-argument): the delete
            // flag and the property atom are distinct xcb parameters.
            const auto           reply = own_reply(
                xcb_get_property_reply( connection_,
                                        xcb_get_property( connection_,
                                                          keepProperty,
                                                          root_,
                                                          check,
                                                          XCB_ATOM_WINDOW,
                                                          noPropertyOffset,
                                                          windowPropertyWords ),
                                        &raw_error )
            );
            // NOLINTEND(readability-suspicious-call-argument)
            const auto error = own_reply( raw_error );
            if( error ==
                nullptr &&
                reply !=
                nullptr &&
                xcb_get_property_value_length( reply.get() ) >=
                static_cast<int>( sizeof( xcb_window_t ) ) )
            {
                xcb_window_t manager = XCB_WINDOW_NONE;
                const void*  value   = xcb_get_property_value( reply.get() );
                std::memcpy( &manager, value, sizeof( manager ) );
                if( manager != XCB_WINDOW_NONE )
                {
                    return true;
                }
            }
        }

        // A window manager that predates EWMH sets no _NET_SUPPORTING_WM_CHECK
        // but still claims the ICCCM manager selection, and it takes input
        // focus just the same.
        return selection_owned( std::string{ managerPrefix } +
                                std::to_string( screen_index_ ) );
    }

    bool
    DisplayProbe::compositor_present() const
    {
        return selection_owned( compositor_selection_name() );
    }

    std::optional<std::string>
    DisplayProbe::accessibility_bus_address() const
    {
        const xcb_atom_t property = existing_atom( atSpiBusProperty );
        if( property == XCB_ATOM_NONE )
        {
            return std::nullopt;
        }
        xcb_generic_error_t* raw_error = nullptr;
        const auto           reply =
            own_reply( xcb_get_property_reply( connection_,
                                               xcb_get_property( connection_,
                                                                 keepProperty,
                                                                 root_,
                                                                 property,
                                                                 XCB_ATOM_STRING,
                                                                 noPropertyOffset,
                                                                 addressPropertyWords ),
                                               &raw_error ) );
        const auto error = own_reply( raw_error );
        if( error != nullptr || reply == nullptr )
        {
            return std::nullopt;
        }
        const int length = xcb_get_property_value_length( reply.get() );
        if( length <= 0 )
        {
            return std::nullopt;
        }
        const auto* const characters =
            static_cast<const char*>( xcb_get_property_value( reply.get() ) );
        return std::string{ characters, static_cast<std::size_t>( length ) };
    }

}    // namespace grab::session
