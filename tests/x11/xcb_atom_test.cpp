#include "platform/x11/xcb_atom.hpp"

// clang-format off
#include "grab/result.hpp"
#include <gtest/gtest.h>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
// clang-format on

namespace
{

    constexpr std::string_view short_atom_name           = "WM_CLASS";
    constexpr auto             short_atom_length         = std::uint16_t{ 8U };
    constexpr std::size_t      oversized_atom_name_extra = 1U;
    constexpr char             atom_name_byte            = 'x';
    constexpr std::uint8_t     xcb_only_if_exists        = 1U;
    constexpr std::uint8_t     xcb_create_if_missing     = 0U;
    constexpr auto atom_name_too_long_error_code = grab::ErrorCode::invalid_argument;
    constexpr std::string_view atom_name_too_long_message = "X atom name is too long";

}    // namespace

TEST( XcbAtom,
      ConvertsValidNameLength )
{
    auto length = grab::platform::x11::atom_name_length( short_atom_name );

    ASSERT_TRUE( length.has_value() );
    EXPECT_EQ( *length, short_atom_length );
}

TEST( XcbAtom,
      RejectsTooLongName )
{
    const auto oversized_length =
        std::numeric_limits<std::uint16_t>::max() + oversized_atom_name_extra;
    const std::string name( oversized_length, atom_name_byte );

    auto              length = grab::platform::x11::atom_name_length( name );

    ASSERT_FALSE( length.has_value() );
    EXPECT_EQ( length.error().code, atom_name_too_long_error_code );
    EXPECT_EQ( length.error().message, atom_name_too_long_message );
}

TEST( XcbAtom,
      MapsModeToXcbOnlyIfExistsFlag )
{
    EXPECT_EQ( grab::platform::x11::atom_only_if_exists(
                   grab::platform::x11::XcbAtomMode::only_if_exists
               ),
               xcb_only_if_exists );
    EXPECT_EQ( grab::platform::x11::atom_only_if_exists(
                   grab::platform::x11::XcbAtomMode::create_if_missing
               ),
               xcb_create_if_missing );
}
