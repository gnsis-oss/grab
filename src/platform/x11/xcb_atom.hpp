#pragma once

#include "grab/result.hpp"
#include "platform/x11/xcb_connection.hpp"
#include "platform/x11/xcb_reply.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <xcb/xcb.h>
#include <xcb/xproto.h>

namespace grab::platform::x11
{

    enum class XcbAtomMode : std::uint8_t
    {
        OnlyIfExists,
        CreateIfMissing,
    };

    [[nodiscard]]
    inline grab::Result<std::uint16_t>
    atom_name_length( std::string_view name )
    {
        if( name.size() > std::numeric_limits<std::uint16_t>::max() )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "X atom name is too long" );
        }
        return static_cast<std::uint16_t>( name.size() );
    }

    [[nodiscard]]
    inline std::uint8_t
    atom_only_if_exists( XcbAtomMode mode ) noexcept
    {
        constexpr std::uint8_t atom_only_if_exists = 1U;
        constexpr std::uint8_t atom_create_missing = 0U;

        switch( mode )
        {
            case XcbAtomMode::OnlyIfExists :
                return atom_only_if_exists;
            case XcbAtomMode::CreateIfMissing :
                return atom_create_missing;
        }
        return atom_create_missing;
    }

    [[nodiscard]]
    inline grab::Result<xcb_atom_t>
    intern_atom( const XcbConnection& conn,
                 std::string_view     name,
                 XcbAtomMode          mode )
    {
        const std::string name_storage{ name };
        auto              name_length = atom_name_length( name_storage );
        if( !name_length.has_value() )
        {
            return grab::fail( name_length.error().code, name_length.error().message );
        }

        const xcb_intern_atom_cookie_t cookie =
            xcb_intern_atom( conn.get(),
                             atom_only_if_exists( mode ),
                             *name_length,
                             name_storage.c_str() );
        xcb_generic_error_t* error_raw = nullptr;
        auto                 reply =
            make_xcb_reply( xcb_intern_atom_reply( conn.get(), cookie, &error_raw ) );
        auto error = make_xcb_reply( error_raw );

        if( error != nullptr || reply == nullptr )
        {
            return grab::fail( grab::ErrorCode::ProviderFailed,
                               "xcb_intern_atom failed" );
        }
        return reply->atom;
    }

}    // namespace grab::platform::x11
