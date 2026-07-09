#include "grab/version.hpp"

#include <gtest/gtest.h>
#include <string_view>

namespace
{

    constexpr std::string_view kExpectedPrefix = "0.1";

}    // namespace

TEST( Version,
      ReportsSemverWithExpectedMajorMinor )
{
    EXPECT_TRUE( grab::version().starts_with( kExpectedPrefix ) );
}
