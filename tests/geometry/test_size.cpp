#include "grab/geometry/size.hpp"

// clang-format off
#include <gtest/gtest.h>
// clang-format on

// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

namespace
{

    namespace geometry = grab::geometry;

}    // namespace

TEST( GeometrySize,
      ComputesAreaAndCompares )
{
    constexpr geometry::Size size{
        .width  = 12U,
        .height = 5U,
    };
    constexpr geometry::Size same{
        .width  = 12U,
        .height = 5U,
    };
    constexpr geometry::Size different{
        .width  = 5U,
        .height = 12U,
    };

    EXPECT_EQ( size.area(), 60U );
    EXPECT_EQ( size, same );
    EXPECT_NE( size, different );
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
