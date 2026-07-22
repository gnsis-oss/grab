#include "grab/enum_table.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <array>
#include <cstdint>
#include <string_view>
// clang-format on

namespace
{

    enum class TestName : std::uint8_t
    {
        Alpha,
        Beta,
        Count,
    };

    constexpr std::string_view alphaText  = "alpha";
    constexpr std::string_view betaText   = "beta";
    constexpr std::string_view fallback   = "fallback";
    constexpr std::string_view missing    = "missing";
    constexpr auto             alphaValue = TestName::Alpha;
    constexpr auto             betaValue  = TestName::Beta;

    inline constexpr auto      testNames  = grab::EnumTable{
        std::to_array( {
            grab::enum_entry( TestName::Alpha, alphaText ),
            grab::enum_entry( TestName::Beta, betaText ),
        } ),
    };
    static_assert( grab::enum_table_has_count( testNames,
                                               TestName::Count ) );

}    // namespace

TEST( EnumTable,
      MapsEnumToText )
{
    static_assert( testNames.text_of( alphaValue, fallback ) == alphaText );

    EXPECT_EQ( testNames.text_of( betaValue, fallback ), betaText );
}

TEST( EnumTable,
      MapsTextToEnum )
{
    constexpr auto alpha = testNames.value_of( alphaText );
    static_assert( alpha.has_value() );
    static_assert( *alpha == alpha );

    const auto beta = testNames.value_of( betaText );
    ASSERT_TRUE( beta.has_value() );
    EXPECT_EQ( *beta, beta );
}

TEST( EnumTable,
      MissingValuesUseExplicitFallbacks )
{
    EXPECT_EQ( testNames.text_of( TestName::Count, fallback ), fallback );
    EXPECT_FALSE( testNames.value_of( missing ).has_value() );
}
