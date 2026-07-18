#include "grab/enum_table.hpp"
#include "grab/payload_fields.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <string_view>
// clang-format on

namespace
{

    constexpr std::string_view keyCodeName    = "key_code";
    constexpr std::string_view buttonNameName = "button_name";
    constexpr std::string_view positionXName  = "position_x";
    constexpr std::string_view positionYName  = "position_y";
    constexpr std::string_view spaceName      = "space";
    constexpr std::string_view stateName      = "state";

    static_assert( grab::enum_table_has_count( grab::detail::payloadFieldNames,
                                               grab::PayloadField::Count ) );

}    // namespace

TEST( PayloadFields,
      NamesCanonicalWireKeys )
{
    EXPECT_EQ( grab::field_name( grab::PayloadField::KeyCode ), keyCodeName );
    EXPECT_EQ( grab::field_name( grab::PayloadField::ButtonName ), buttonNameName );
    EXPECT_EQ( grab::field_name( grab::PayloadField::PositionX ), positionXName );
    EXPECT_EQ( grab::field_name( grab::PayloadField::PositionY ), positionYName );
    EXPECT_EQ( grab::field_name( grab::PayloadField::Space ), spaceName );
    EXPECT_EQ( grab::field_name( grab::PayloadField::State ), stateName );
}
