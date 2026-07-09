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
        alpha,
        beta,
        count,
    };

    constexpr std::string_view kAlphaText = "alpha";
    constexpr std::string_view kBetaText  = "beta";
    constexpr std::string_view kFallback  = "fallback";
    constexpr std::string_view kMissing   = "missing";
    constexpr auto             kAlpha     = TestName::alpha;
    constexpr auto             kBeta      = TestName::beta;

    inline constexpr auto      kTestNames = grab::EnumTable{
        std::to_array( {
            grab::enum_entry( TestName::alpha, kAlphaText ),
            grab::enum_entry( TestName::beta, kBetaText ),
        } ),
    };
    static_assert( grab::enum_table_has_count( kTestNames,
                                               TestName::count ) );

}    // namespace

TEST( EnumTable,
      MapsEnumToText )
{
    static_assert( kTestNames.text_of( kAlpha, kFallback ) == kAlphaText );

    EXPECT_EQ( kTestNames.text_of( kBeta, kFallback ), kBetaText );
}

TEST( EnumTable,
      MapsTextToEnum )
{
    constexpr auto alpha = kTestNames.value_of( kAlphaText );
    static_assert( alpha.has_value() );
    static_assert( *alpha == kAlpha );

    const auto beta = kTestNames.value_of( kBetaText );
    ASSERT_TRUE( beta.has_value() );
    EXPECT_EQ( *beta, kBeta );
}

TEST( EnumTable,
      MissingValuesUseExplicitFallbacks )
{
    EXPECT_EQ( kTestNames.text_of( TestName::count, kFallback ), kFallback );
    EXPECT_FALSE( kTestNames.value_of( kMissing ).has_value() );
}
