#include "grab/version.hpp"

#include <gtest/gtest.h>
#include <string>
#include <string_view>

namespace
{

    constexpr std::string_view kExpectedVersion = "0.0.1";
    constexpr int              kExpectedMajor   = 0;
    constexpr int              kExpectedMinor   = 0;
    constexpr int              kExpectedPatch   = 1;

}    // namespace

TEST( Version,
      MatchesCMakeProjectVersion )
{
    // Source of truth is `project(grab VERSION x.y.z)`; these must track it.
    EXPECT_EQ( grab::version, kExpectedVersion );
    EXPECT_EQ( grab::version_major, kExpectedMajor );
    EXPECT_EQ( grab::version_minor, kExpectedMinor );
    EXPECT_EQ( grab::version_patch, kExpectedPatch );
}

TEST( Version,
      StringMatchesComponents )
{
    const std::string expected = std::to_string( grab::version_major ) +
                                 "." +
                                 std::to_string( grab::version_minor ) +
                                 "." +
                                 std::to_string( grab::version_patch );
    EXPECT_EQ( grab::version, expected );
}
