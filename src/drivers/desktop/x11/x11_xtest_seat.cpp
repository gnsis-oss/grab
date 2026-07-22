#include "drivers/desktop/x11/x11_xtest_seat.hpp"
#include "grab/result.hpp"

#include <cstdint>
#include <cstdlib>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <xcb/xcb.h>
#include <xcb/xproto.h>
#include <xcb/xtest.h>

namespace grab::input
{

    namespace
    {

        constexpr std::string_view xtestExtensionName = "XTEST";
        constexpr int              xcbOk              = 0;
        constexpr std::uint8_t     currentDevice      = 0U;
        constexpr std::uint8_t     noDetail           = 0U;
        constexpr std::int16_t     noRootX            = 0;
        constexpr std::int16_t     noRootY            = 0;

        template<typename T>
        using XcbOwned = std::unique_ptr<T, decltype( &std::free )>;

        using XcbConnection =
            std::unique_ptr<xcb_connection_t, decltype( &xcb_disconnect )>;

        template<typename T>
        [[nodiscard]]
        XcbOwned<T>
        take_xcb_owned( T* pointer ) noexcept
        {
            return XcbOwned<T>{ pointer, &std::free };
        }

        [[nodiscard]]
        grab::Result<void>
        fail_if_connection_closed( const xcb_connection_t* connection )
        {
            if( connection == nullptr )
            {
                return grab::fail( grab::ErrorCode::DeviceInaccessible,
                                   "XCB seat connection is not open" );
            }
            return {};
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
        grab::Result<void>
        require_xtest( xcb_connection_t* connection )
        {
            const auto extension_cookie = xcb_query_extension(
                connection,
                static_cast<std::uint16_t>( xtestExtensionName.size() ),
                xtestExtensionName.data()
            );
            xcb_generic_error_t* raw_extension_error = nullptr;
            const auto           extension_reply =
                take_xcb_owned( xcb_query_extension_reply( connection,
                                                           extension_cookie,
                                                           &raw_extension_error ) );
            const auto extension_error = take_xcb_owned( raw_extension_error );

            if( extension_error !=
                nullptr ||
                extension_reply ==
                nullptr ||
                extension_reply->present == 0U )
            {
                return grab::fail( grab::ErrorCode::DeviceInaccessible,
                                   "XTEST extension is unavailable" );
            }

            xcb_generic_error_t* raw_version_error = nullptr;
            const auto version_cookie = xcb_test_get_version( connection,
                                                              XCB_TEST_MAJOR_VERSION,
                                                              XCB_TEST_MINOR_VERSION );
            const auto version_reply =
                take_xcb_owned( xcb_test_get_version_reply( connection,
                                                            version_cookie,
                                                            &raw_version_error ) );
            const auto version_error = take_xcb_owned( raw_version_error );

            if( version_error != nullptr || version_reply == nullptr )
            {
                return grab::fail( grab::ErrorCode::DeviceInaccessible,
                                   "XTEST version query failed" );
            }

            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        fake_input( xcb_connection_t* connection,
                    std::uint32_t     root,
                    std::uint8_t      type,
                    std::uint8_t      detail,
                    std::int16_t      root_x,
                    std::int16_t      root_y,
                    std::string_view  operation )
        {
            auto open_result = fail_if_connection_closed( connection );
            if( !open_result.has_value() )
            {
                return open_result;
            }

            return check_request( connection,
                                  xcb_test_fake_input_checked( connection,
                                                               type,
                                                               detail,
                                                               XCB_CURRENT_TIME,
                                                               root,
                                                               root_x,
                                                               root_y,
                                                               currentDevice ),
                                  operation );
        }

    }    // namespace

    Seat::Seat( xcb_connection_t* connection,
                std::uint32_t     root ) noexcept :
        connection_( connection ),
        root_( root )
    {
    }

    Seat::~Seat()
    {
        if( connection_ != nullptr )
        {
            xcb_disconnect( connection_ );
        }
    }

    Seat::Seat( Seat&& other ) noexcept :
        connection_( std::exchange( other.connection_,
                                    nullptr ) ),
        root_( std::exchange( other.root_,
                              0 ) )
    {
    }

    Seat&
    Seat::operator=( Seat&& other ) noexcept
    {
        if( this != &other )
        {
            if( connection_ != nullptr )
            {
                xcb_disconnect( connection_ );
            }
            connection_ = std::exchange( other.connection_, nullptr );
            root_       = std::exchange( other.root_, 0 );
        }
        return *this;
    }

    grab::Result<Seat>
    Seat::open( const char* display )
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

        auto xtest_result = require_xtest( connection.get() );
        if( !xtest_result.has_value() )
        {
            return std::unexpected( std::move( xtest_result.error() ) );
        }

        auto root_result = default_root( connection.get(), screen_index );
        if( !root_result.has_value() )
        {
            return std::unexpected( std::move( root_result.error() ) );
        }

        return Seat{ connection.release(), *root_result };
    }

    grab::Result<void>
    Seat::move_pointer_absolute( std::int16_t x,
                                 std::int16_t y )
    {
        return fake_input( connection_,
                           root_,
                           XCB_MOTION_NOTIFY,
                           noDetail,
                           x,
                           y,
                           "XTEST pointer move" );
    }

    grab::Result<void>
    Seat::button( std::uint8_t button_detail,
                  bool         press )
    {
        if( button_detail == 0U )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "XTEST button detail must be nonzero" );
        }

        return fake_input( connection_,
                           root_,
                           press ? XCB_BUTTON_PRESS : XCB_BUTTON_RELEASE,
                           button_detail,
                           noRootX,
                           noRootY,
                           "XTEST button event" );
    }

    grab::Result<void>
    Seat::key( std::uint8_t keycode,
               bool         press )
    {
        if( keycode == 0U )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "XTEST keycode must be nonzero" );
        }

        return fake_input( connection_,
                           root_,
                           press ? XCB_KEY_PRESS : XCB_KEY_RELEASE,
                           keycode,
                           noRootX,
                           noRootY,
                           "XTEST key event" );
    }

    grab::Result<void>
    Seat::flush()
    {
        auto open_result = fail_if_connection_closed( connection_ );
        if( !open_result.has_value() )
        {
            return open_result;
        }

        if( xcb_flush( connection_ ) <=
            0 ||
            xcb_connection_has_error( connection_ ) != xcbOk )
        {
            return grab::fail( grab::ErrorCode::DeviceInaccessible,
                               "XCB seat flush failed" );
        }

        return {};
    }

}    // namespace grab::input
