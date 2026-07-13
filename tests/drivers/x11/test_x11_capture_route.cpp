#include "drivers/desktop/x11/x11_capture_route.hpp"
#include "grab/capture.hpp"
#include "screen/enumerate.hpp"

#include <gtest/gtest.h>

TEST( X11CaptureRoute,
      DISABLED_CaptureOutputProducesFrame )
{
    auto route = grab::drivers::desktop::x11::X11CaptureRoute::open();
    ASSERT_TRUE( route.has_value() );

    const auto outputs = grab::screen::list_outputs();
    ASSERT_TRUE( outputs.has_value() );
    ASSERT_FALSE( outputs->empty() );

    auto frame = route->capture_output( outputs->front().name );
    ASSERT_TRUE( frame.has_value() );
    const auto output =
        route->coordinate_authority().output_space( outputs->front().name );
    ASSERT_TRUE( output.has_value() );
    EXPECT_NE( frame->id.value, 0U );
    EXPECT_EQ( frame->space, output->space );
    EXPECT_EQ( frame->generation, output->generation );
    EXPECT_EQ( frame->content_rect.space, output->space );
    EXPECT_DOUBLE_EQ( frame->content_rect.x, 0.0 );
    EXPECT_DOUBLE_EQ( frame->content_rect.y, 0.0 );
    EXPECT_DOUBLE_EQ( frame->scale, output->scale );
}
