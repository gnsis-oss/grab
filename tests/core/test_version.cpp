#include "grab/version.hpp"

#include <gtest/gtest.h>
#include <string>
#include <string_view>

namespace
{

    constexpr std::string_view expectedVersion = "0.1.0";
    constexpr int              expectedMajor   = 0;
    constexpr int              expectedMinor   = 1;
    constexpr int              expectedPatch   = 0;

}    // namespace

TEST( Version,
      MatchesCMakeProjectVersion )
{
    // Source of truth is `project(grab VERSION x.y.z)`; these must track it.
    EXPECT_EQ( grab::version, expectedVersion );
    EXPECT_EQ( grab::majorVersion, expectedMajor );
    EXPECT_EQ( grab::minorVersion, expectedMinor );
    EXPECT_EQ( grab::patchVersion, expectedPatch );
}

TEST( Version,
      StringMatchesComponents )
{
    const std::string expected = std::to_string( grab::majorVersion ) +
                                 "." +
                                 std::to_string( grab::minorVersion ) +
                                 "." +
                                 std::to_string( grab::patchVersion );
    EXPECT_EQ( grab::version, expected );
}
