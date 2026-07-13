#include "drivers/desktop/x11/x11_runtime.hpp"
#include "grab/context.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstdint>
#include <string_view>
// clang-format on

namespace
{

    constexpr std::uint32_t initial_generation = 1U;

}    // namespace

TEST( X11Runtime,
      ExposesIdentityBeforeStart )
{
    const grab::drivers::desktop::x11::X11Runtime runtime;

    EXPECT_EQ( runtime.name(), std::string_view{ "x11" } );
    EXPECT_EQ( runtime.generation(), initial_generation );
}

// Not runnable here without a real X server; enable this test under Xvfb.
TEST( X11Runtime,
      DISABLED_StartAndRestartRequireXvfb )
{
    grab::drivers::desktop::x11::X11Runtime runtime;
    const grab::OperationContext            context{
        .deadline = grab::Deadline::unbounded(),
    };

    ASSERT_TRUE( runtime.start( context ).has_value() );
    ASSERT_TRUE( runtime.stop().has_value() );
    ASSERT_TRUE( runtime.start( context ).has_value() );
    EXPECT_EQ( runtime.generation(), initial_generation + 1U );
    EXPECT_TRUE( runtime.stop().has_value() );
}
