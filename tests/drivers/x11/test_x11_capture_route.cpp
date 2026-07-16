#include "drivers/desktop/x11/enumerate.hpp"
#include "drivers/desktop/x11/x11_capture_route.hpp"
#include "grab/capture.hpp"
#include "grab/space.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdlib>
#include <string_view>
#include <vector>
// clang-format on

TEST( X11CaptureRoute,
      CaptureOutputProducesFrame )
{
    const char* const display = std::getenv( "DISPLAY" );
    if( display == nullptr || std::string_view{ display }.empty() )
    {
        GTEST_SKIP() << "requires Xvfb (DISPLAY is not set)";
    }

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

TEST( X11CaptureRoute,
      RefreshTransformsExportsOutputToGlobalRecords )
{
    const char* const display = std::getenv( "DISPLAY" );
    if( display == nullptr || std::string_view{ display }.empty() )
    {
        GTEST_SKIP() << "requires Xvfb (DISPLAY is not set)";
    }

    auto route = grab::drivers::desktop::x11::X11CaptureRoute::open();
    ASSERT_TRUE( route.has_value() );

    const auto outputs = grab::screen::list_outputs();
    ASSERT_TRUE( outputs.has_value() );
    ASSERT_FALSE( outputs->empty() );

    const auto transforms = route->refresh_transforms();
    ASSERT_TRUE( transforms.has_value() );
    const std::vector<grab::TransformRecord>& records = *transforms;
    ASSERT_FALSE( records.empty() );
    EXPECT_EQ( records.size(), outputs->size() );

    const auto output =
        route->coordinate_authority().output_space( outputs->front().name );
    ASSERT_TRUE( output.has_value() );

    const auto transform = std::ranges::find_if(
        records,
        [source = output->space]( const grab::TransformRecord& candidate )
        {
            return candidate.source == source;
        }
    );
    ASSERT_NE( transform, records.end() );
    EXPECT_EQ( transform->destination, route->global_space() );
    EXPECT_EQ( transform->trust, grab::TransformTrust::Exact );
}

TEST( X11CaptureRoute,
      CaptureDisplayProducesFrame )
{
    const char* const display = std::getenv( "DISPLAY" );
    if( display == nullptr || std::string_view{ display }.empty() )
    {
        GTEST_SKIP() << "requires Xvfb (DISPLAY is not set)";
    }

    auto route = grab::drivers::desktop::x11::X11CaptureRoute::open();
    ASSERT_TRUE( route.has_value() );

    auto frame = route->capture_display();
    ASSERT_TRUE( frame.has_value() );
    EXPECT_NE( frame->id.value, 0U );
    EXPECT_EQ( frame->space, route->global_space() );
    EXPECT_EQ( frame->generation, route->coordinate_authority().capture_generation() );
    EXPECT_GT( frame->image.width, 0U );
    EXPECT_GT( frame->image.height, 0U );
}

TEST( X11CaptureRoute,
      CaptureRegionProducesFrame )
{
    const char* const display = std::getenv( "DISPLAY" );
    if( display == nullptr || std::string_view{ display }.empty() )
    {
        GTEST_SKIP() << "requires Xvfb (DISPLAY is not set)";
    }

    auto route = grab::drivers::desktop::x11::X11CaptureRoute::open();
    ASSERT_TRUE( route.has_value() );

    auto frame = route->capture_region( 0, 0, 64, 48 );
    ASSERT_TRUE( frame.has_value() );
    EXPECT_EQ( frame->image.width, 64U );
    EXPECT_EQ( frame->image.height, 48U );
    EXPECT_EQ( frame->space, route->global_space() );
    EXPECT_EQ( frame->generation, route->coordinate_authority().capture_generation() );
    EXPECT_EQ( frame->content_rect.space, route->global_space() );
}
