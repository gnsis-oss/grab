#include "drivers/desktop/x11/x11_runtime.hpp"
#include "grab/context.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstdint>
#include <cstdlib>
#include <string_view>
// clang-format on

namespace
{

    constexpr std::uint32_t initial_generation   = 1U;
    constexpr std::uint32_t generation_increment = 1U;

}    // namespace

TEST( X11Runtime,
      ExposesIdentityBeforeStart )
{
    const grab::drivers::desktop::x11::X11Runtime runtime;

    EXPECT_EQ( runtime.name(), std::string_view{ "x11" } );
    EXPECT_EQ( runtime.generation(), initial_generation );
}

TEST( X11Runtime,
      CaptureRouteIsNullBeforeStart )
{
    grab::drivers::desktop::x11::X11Runtime runtime;

    EXPECT_EQ( runtime.capture_route(), nullptr );
    EXPECT_EQ( runtime.capture_route_error(), nullptr );
}

TEST( X11Runtime,
      StartAndRestart )
{
    const char* const display = std::getenv( "DISPLAY" );
    if( display == nullptr || std::string_view{ display }.empty() )
    {
        GTEST_SKIP() << "requires Xvfb (DISPLAY is not set)";
    }

    grab::drivers::desktop::x11::X11Runtime runtime;
    const grab::OperationContext            context{
        .deadline = grab::Deadline::unbounded(),
    };

    ASSERT_TRUE( runtime.start( context ).has_value() );
    ASSERT_TRUE( runtime.stop().has_value() );
    ASSERT_TRUE( runtime.start( context ).has_value() );
    EXPECT_EQ( runtime.generation(), initial_generation + generation_increment );
    EXPECT_TRUE( runtime.stop().has_value() );
}

TEST( X11Runtime,
      StartOpensCaptureRouteOnXvfb )
{
    const char* const display = std::getenv( "DISPLAY" );
    if( display == nullptr || std::string_view{ display }.empty() )
    {
        GTEST_SKIP() << "requires Xvfb (DISPLAY is not set)";
    }

    grab::drivers::desktop::x11::X11Runtime runtime;
    const grab::OperationContext            context{
        .deadline = grab::Deadline::unbounded(),
    };

    ASSERT_TRUE( runtime.start( context ).has_value() );
    EXPECT_NE( runtime.capture_route(), nullptr );
    EXPECT_EQ( runtime.capture_route_error(), nullptr );
    ASSERT_TRUE( runtime.stop().has_value() );
    EXPECT_EQ( runtime.capture_route(), nullptr );
}

TEST( X11Runtime,
      StartExposesEventAndTopologySources )
{
    const char* const display = std::getenv( "DISPLAY" );
    if( display == nullptr || std::string_view{ display }.empty() )
    {
        GTEST_SKIP() << "requires Xvfb (DISPLAY is not set)";
    }

    grab::drivers::desktop::x11::X11Runtime runtime;
    const grab::OperationContext            context{
        .deadline = grab::Deadline::unbounded(),
    };

    ASSERT_TRUE( runtime.start( context ).has_value() );
    EXPECT_NE( runtime.event_source(), nullptr );
    EXPECT_NE( runtime.topology_source(), nullptr );
    EXPECT_NE( runtime.native_seat(), nullptr );
    ASSERT_TRUE( runtime.stop().has_value() );
    EXPECT_EQ( runtime.event_source(), nullptr );
    EXPECT_EQ( runtime.topology_source(), nullptr );
}
