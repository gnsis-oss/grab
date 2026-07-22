#include "drivers/desktop/x11/xcb_connection.hpp"
#include "drivers/desktop/x11/xcb_reply.hpp"
#include "drivers/desktop/x11/xi_seat.hpp"
#include "grab/result.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <xcb/xcb.h>
#include <xcb/xinput.h>

namespace grab::platform::x11
{
    namespace
    {

        constexpr std::uint8_t  send_core_events  = 1U;
        constexpr std::uint8_t  enable_device     = 1U;
        constexpr std::uint8_t  one_change        = 1U;
        constexpr std::uint32_t alignment_mask    = 3U;
        constexpr std::uint32_t alignment_unit    = 4U;
        constexpr std::uint16_t invalid_device_id = 0U;
        constexpr int           fp1616_shift      = 16;
        constexpr double        fp1616_divisor    = 65'536.0;

        [[nodiscard]]
        xcb_input_fp1616_t
        to_fp1616( std::int16_t value ) noexcept
        {
            return static_cast<xcb_input_fp1616_t>( value ) << fp1616_shift;
        }

        [[nodiscard]]
        double
        from_fp1616( xcb_input_fp1616_t value ) noexcept
        {
            return static_cast<double>( value ) / fp1616_divisor;
        }

    }    // namespace

    XiSeat::XiSeat( const XcbConnection& conn,
                    std::uint16_t        pointer_id,
                    std::uint16_t        keyboard_id,
                    std::uint16_t        primary_pointer_id ) noexcept :
        conn( &conn ),
        pointer_device_id( pointer_id ),
        keyboard_device_id( keyboard_id ),
        primary_pointer_device_id( primary_pointer_id ),
        active( true )
    {
    }

    XiSeat::XiSeat( XiSeat&& other ) noexcept :
        conn( other.conn ),
        pointer_device_id( other.pointer_device_id ),
        keyboard_device_id( other.keyboard_device_id ),
        primary_pointer_device_id( other.primary_pointer_device_id ),
        active( other.active )
    {
        other.active = false;
        other.conn   = nullptr;
    }

    XiSeat&
    XiSeat::operator=( XiSeat&& other ) noexcept
    {
        if( this != &other )
        {
            remove();
            conn                      = other.conn;
            pointer_device_id         = other.pointer_device_id;
            keyboard_device_id        = other.keyboard_device_id;
            primary_pointer_device_id = other.primary_pointer_device_id;
            active                    = other.active;
            other.active              = false;
            other.conn                = nullptr;
        }
        return *this;
    }

    XiSeat::~XiSeat()
    {
        remove();
    }

    grab::Result<XiSeat>
    XiSeat::create( const XcbConnection& conn,
                    std::string_view     name )
    {
        const std::size_t header_size = sizeof( xcb_input_add_master_t );
        const std::size_t unpadded    = header_size + name.size();
        const std::size_t padded =
            ( unpadded + alignment_mask ) & ~static_cast<std::size_t>( alignment_mask );

        xcb_input_add_master_t header{};
        header.type      = XCB_INPUT_HIERARCHY_CHANGE_TYPE_ADD_MASTER;
        header.len       = static_cast<std::uint16_t>( padded / alignment_unit );
        header.name_len  = static_cast<std::uint16_t>( name.size() );
        header.send_core = send_core_events;
        header.enable    = enable_device;

        std::vector<std::uint8_t> change( padded, 0U );
        std::memcpy( change.data(), &header, header_size );
        if( !name.empty() )
        {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            std::memcpy( change.data() + header_size, name.data(), name.size() );
        }

        const xcb_void_cookie_t cookie = xcb_input_xi_change_hierarchy_checked(
            conn.get(),
            one_change,
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            reinterpret_cast<const xcb_input_hierarchy_change_t*>( change.data() )
        );
        const auto error = make_xcb_reply( xcb_request_check( conn.get(), cookie ) );
        if( error )
        {
            return grab::fail( grab::ErrorCode::ProviderFailed,
                               "XIChangeHierarchy AddMaster failed" );
        }

        // Resolve the created master ids by name; capture a primary master
        // pointer that is not ours for isolation checks.
        xcb_generic_error_t*                     query_error = nullptr;
        const xcb_input_xi_query_device_cookie_t query_cookie =
            xcb_input_xi_query_device( conn.get(), XCB_INPUT_DEVICE_ALL_MASTER );
        auto reply = make_xcb_reply(
            xcb_input_xi_query_device_reply( conn.get(), query_cookie, &query_error )
        );
        const auto owned_error = make_xcb_reply( query_error );
        if( !reply )
        {
            return grab::fail( grab::ErrorCode::ProviderFailed,
                               "XIQueryDevice failed after AddMaster" );
        }

        std::uint16_t                       pointer_id  = invalid_device_id;
        std::uint16_t                       keyboard_id = invalid_device_id;
        std::uint16_t                       primary     = invalid_device_id;

        xcb_input_xi_device_info_iterator_t iter =
            xcb_input_xi_query_device_infos_iterator( reply.get() );
        for( ; iter.rem > 0; xcb_input_xi_device_info_next( &iter ) )
        {
            const xcb_input_xi_device_info_t* info = iter.data;
            const std::string_view            device_name{
                xcb_input_xi_device_info_name( info ),
                static_cast<std::size_t>( xcb_input_xi_device_info_name_length( info ) )
            };
            const bool ours = device_name.starts_with( name );

            if( info->type == XCB_INPUT_DEVICE_TYPE_MASTER_POINTER )
            {
                if( ours )
                {
                    pointer_id = info->deviceid;
                }
                else if( primary == invalid_device_id )
                {
                    primary = info->deviceid;
                }
            }
            else if( info->type == XCB_INPUT_DEVICE_TYPE_MASTER_KEYBOARD && ours )
            {
                keyboard_id = info->deviceid;
            }
        }

        if( pointer_id == invalid_device_id || keyboard_id == invalid_device_id )
        {
            return grab::fail( grab::ErrorCode::ProviderFailed,
                               "created master seat not found in device list" );
        }

        return XiSeat{ conn, pointer_id, keyboard_id, primary };
    }

    void
    XiSeat::remove() noexcept
    {
        if( !active || conn == nullptr )
        {
            return;
        }

        xcb_input_remove_master_t change{};
        change.type = XCB_INPUT_HIERARCHY_CHANGE_TYPE_REMOVE_MASTER;
        change.len  = static_cast<std::uint16_t>( sizeof( xcb_input_remove_master_t ) /
                                                  alignment_unit );
        change.deviceid                = pointer_device_id;
        change.return_mode             = XCB_INPUT_CHANGE_MODE_FLOAT;
        change.return_pointer          = invalid_device_id;
        change.return_keyboard         = invalid_device_id;

        const xcb_void_cookie_t cookie = xcb_input_xi_change_hierarchy_checked(
            conn->get(),
            one_change,
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            reinterpret_cast<const xcb_input_hierarchy_change_t*>( &change )
        );
        // Best-effort removal in a noexcept destructor: the checked round-trip
        // forces the server to process the removal before the connection is torn
        // down (an unchecked flush can be dropped on disconnect and leak the
        // master). The error, if any, cannot be surfaced from here.
        const auto error = make_xcb_reply( xcb_request_check( conn->get(), cookie ) );
        ( void )error;

        active = false;
        conn   = nullptr;
    }

    std::uint16_t
    XiSeat::pointer_id() const noexcept
    {
        return pointer_device_id;
    }

    std::uint16_t
    XiSeat::keyboard_id() const noexcept
    {
        return keyboard_device_id;
    }

    std::uint16_t
    XiSeat::primary_pointer_id() const noexcept
    {
        return primary_pointer_device_id;
    }

    grab::Result<void>
    XiSeat::warp_to( std::int16_t x,
                     std::int16_t y )
    {
        const xcb_void_cookie_t cookie =
            xcb_input_xi_warp_pointer_checked( conn->get(),
                                               XCB_NONE,
                                               conn->root(),
                                               0,
                                               0,
                                               0,
                                               0,
                                               to_fp1616( x ),
                                               to_fp1616( y ),
                                               pointer_device_id );
        const auto error = make_xcb_reply( xcb_request_check( conn->get(), cookie ) );
        if( error )
        {
            return grab::fail( grab::ErrorCode::ProviderFailed, "XIWarpPointer failed" );
        }
        return {};
    }

    grab::Result<XiSeat::PointerPos>
    XiSeat::query( std::uint16_t device_id ) const
    {
        xcb_generic_error_t*                      error_raw = nullptr;
        const xcb_input_xi_query_pointer_cookie_t cookie =
            xcb_input_xi_query_pointer( conn->get(), conn->root(), device_id );
        auto reply = make_xcb_reply(
            xcb_input_xi_query_pointer_reply( conn->get(), cookie, &error_raw )
        );
        const auto error = make_xcb_reply( error_raw );
        if( error || !reply )
        {
            return grab::fail( grab::ErrorCode::GeometryUntrusted,
                               "XIQueryPointer position is unavailable" );
        }
        return PointerPos{
            .x = from_fp1616( reply->root_x ),
            .y = from_fp1616( reply->root_y ),
        };
    }

}    // namespace grab::platform::x11
