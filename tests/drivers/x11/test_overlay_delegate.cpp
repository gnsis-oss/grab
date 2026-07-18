#include "drivers/desktop/x11/overlay_delegate.hpp"
#include "drivers/desktop/x11/x11_runtime.hpp"
#include "drivers/desktop/x11/xcb_connection.hpp"
#include "grab/capability.hpp"
#include "grab/context.hpp"
#include "grab/overlay.hpp"
#include "grab/result.hpp"
#include "grab/space.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <xcb/shape.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>
// clang-format on

namespace
{

    using grab::drivers::desktop::x11::X11OverlayDelegate;
    namespace x11_detail         = grab::drivers::desktop::x11::detail;

    constexpr auto fadeDuration  = std::chrono::milliseconds{ 1'000 };
    constexpr auto ttlDuration   = std::chrono::milliseconds{ 750 };
    constexpr auto shapeStart    = std::chrono::milliseconds{ 100 };
    constexpr auto evaluationNow = std::chrono::milliseconds{ 500 };
    constexpr auto fadeDeadline  = std::chrono::milliseconds{ 1'100 };
    constexpr auto ttlDeadline   = std::chrono::milliseconds{ 850 };

    [[nodiscard]]
    bool
    display_is_available()
    {
        // Matches the existing X11 DISPLAY-gated fixture convention.
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        const char* const display = std::getenv( "DISPLAY" );
        return display != nullptr && !std::string_view{ display }.empty();
    }

    [[nodiscard]]
    grab::overlay::ShapeRecord
    fading_record()
    {
        return grab::overlay::ShapeRecord{
            .id = { .epoch = { .value = 1U }, .slot = 1U },
            .shape =
                {
                   .geometry = grab::overlay::Rect(),
                   .stroke   = grab::overlay::StrokeStyle(),
                   .fill     = {},
                   .lifetime =
                        grab::overlay::Fade{
                            .duration = fadeDuration,
                        }, },
            .started_at = shapeStart,
        };
    }

}    // namespace

TEST( X11OverlayProbe,
      SelectsOrderedDistinctCapabilityReasons )
{
    constexpr x11_detail::OverlayProbePrerequisites nothing{};
    constexpr x11_detail::OverlayProbePrerequisites noXfixes{
        .argb32_visual = true,
    };
    constexpr x11_detail::OverlayProbePrerequisites noCompositor{
        .argb32_visual      = true,
        .xfixes_shape_input = true,
    };
    constexpr x11_detail::OverlayProbePrerequisites available{
        .argb32_visual      = true,
        .xfixes_shape_input = true,
        .compositor_owner   = true,
    };

    const auto argb_reason       = x11_detail::overlay_probe_reason( nothing );
    const auto xfixes_reason     = x11_detail::overlay_probe_reason( noXfixes );
    const auto compositor_reason = x11_detail::overlay_probe_reason( noCompositor );

    ASSERT_TRUE( argb_reason.has_value() );
    ASSERT_TRUE( xfixes_reason.has_value() );
    ASSERT_TRUE( compositor_reason.has_value() );
    EXPECT_NE( *argb_reason, *xfixes_reason );
    EXPECT_NE( *argb_reason, *compositor_reason );
    EXPECT_NE( *xfixes_reason, *compositor_reason );
    EXPECT_TRUE( argb_reason->contains( "ARGB32" ) );
    EXPECT_TRUE( xfixes_reason->contains( "XFixes ShapeInput" ) );
    EXPECT_TRUE( compositor_reason->contains( "compositing manager" ) );
    EXPECT_FALSE( x11_detail::overlay_probe_reason( available ).has_value() );
}

TEST( X11OverlayDamagePlan,
      LiveFadeContinuesFrameDamageWithoutSceneChanges )
{
    const std::array shapes{ fading_record() };

    const auto plan = x11_detail::overlay_damage_plan( shapes, evaluationNow, false );

    EXPECT_TRUE( plan.render_frame );
    EXPECT_TRUE( plan.continue_fade );
    ASSERT_TRUE( plan.next_lifetime_deadline.has_value() );
    EXPECT_EQ( *plan.next_lifetime_deadline, fadeDeadline );
}

TEST( X11OverlayDamagePlan,
      TtlSchedulesItsDeadlineWithoutContinuousFrames )
{
    auto ttl           = fading_record();
    ttl.shape.lifetime = grab::overlay::Ttl{
        .duration = ttlDuration,
    };
    const std::array shapes{ ttl };

    const auto plan = x11_detail::overlay_damage_plan( shapes, evaluationNow, false );

    EXPECT_FALSE( plan.render_frame );
    EXPECT_FALSE( plan.continue_fade );
    ASSERT_TRUE( plan.next_lifetime_deadline.has_value() );
    EXPECT_EQ( *plan.next_lifetime_deadline, ttlDeadline );
}

TEST( X11OverlayDelegate,
      BareXvfbFailsOpenWithCompositorReason )
{
    if( !display_is_available() )
    {
        GTEST_SKIP() << "requires Xvfb (DISPLAY is not set)";
    }

    auto delegate = X11OverlayDelegate::create();
    ASSERT_TRUE( delegate.has_value() ) << delegate.error().message;

    const auto opened = ( *delegate )->open( grab::CoordinateSpaceId{ 1U } );
    ASSERT_FALSE( opened.has_value() );
    EXPECT_EQ( opened.error().code, grab::ErrorCode::CapabilityUnavailable );
    EXPECT_TRUE( opened.error().message.contains( "compositing manager" ) );
    ( *delegate )->close();
}

TEST( X11OverlayCapability,
      BareXvfbDoesNotAdvertiseOverlayRow )
{
    if( !display_is_available() )
    {
        GTEST_SKIP() << "requires Xvfb (DISPLAY is not set)";
    }

    grab::drivers::desktop::x11::X11Runtime runtime;
    ASSERT_TRUE( runtime.start( grab::OperationContext{} ).has_value() );

    const auto rows = runtime.capabilities();
    EXPECT_EQ( std::ranges::find( rows, grab::Capability::Overlay ), rows.end() );
    const auto* const reason = runtime.overlay_delegate_error();
    ASSERT_NE( reason, nullptr );
    EXPECT_EQ( reason->code, grab::ErrorCode::CapabilityUnavailable );
    EXPECT_TRUE( reason->message.contains( "compositing manager" ) );

    EXPECT_TRUE( runtime.stop().has_value() );
}

TEST( X11OverlayDelegate,
      InputPassthroughProducesEmptyShapeInputRegion )
{
    if( !display_is_available() )
    {
        GTEST_SKIP() << "requires Xvfb (DISPLAY is not set)";
    }

    auto connection = grab::platform::x11::XcbConnection::open( "" );
    ASSERT_TRUE( connection.has_value() );
    auto* const             raw        = connection->get();
    const auto              window     = xcb_generate_id( raw );
    constexpr std::uint16_t testExtent = 32U;
    const auto created = xcb_create_window_checked( raw,
                                                    XCB_COPY_FROM_PARENT,
                                                    window,
                                                    connection->root(),
                                                    0,
                                                    0,
                                                    testExtent,
                                                    testExtent,
                                                    0U,
                                                    XCB_WINDOW_CLASS_INPUT_OUTPUT,
                                                    XCB_COPY_FROM_PARENT,
                                                    0U,
                                                    nullptr );
    std::unique_ptr<xcb_generic_error_t, decltype( &std::free )> create_error{
        xcb_request_check( raw, created ),
        &std::free,
    };
    ASSERT_EQ( create_error, nullptr );

    ASSERT_TRUE( x11_detail::apply_input_passthrough( raw, window ).has_value() );
    xcb_generic_error_t* raw_error{};
    std::unique_ptr<xcb_shape_get_rectangles_reply_t, decltype( &std::free )> reply{
        xcb_shape_get_rectangles_reply(
            raw,
            xcb_shape_get_rectangles( raw, window, XCB_SHAPE_SK_INPUT ),
            &raw_error
        ),
        &std::free,
    };
    std::unique_ptr<xcb_generic_error_t, decltype( &std::free )> error{
        raw_error,
        &std::free,
    };
    ASSERT_EQ( error, nullptr );
    ASSERT_NE( reply, nullptr );
    EXPECT_EQ( xcb_shape_get_rectangles_rectangles_length( reply.get() ), 0 );

    xcb_destroy_window( raw, window );
    xcb_flush( raw );
}

TEST( X11OverlayDelegate,
      UnmappedTestOpenCloseOpenCycleIsSafe )
{
    if( !display_is_available() )
    {
        GTEST_SKIP() << "requires Xvfb (DISPLAY is not set)";
    }

    auto delegate = X11OverlayDelegate::create();
    ASSERT_TRUE( delegate.has_value() ) << delegate.error().message;

    ASSERT_TRUE( x11_detail::X11OverlayDelegateTestAccess::open_unmapped(
                     **delegate,
                     grab::CoordinateSpaceId{ 1U }
    )
                     .has_value() );
    EXPECT_NE( x11_detail::X11OverlayDelegateTestAccess::window( **delegate ),
               XCB_WINDOW_NONE );
    ( *delegate )->close();

    ASSERT_TRUE( x11_detail::X11OverlayDelegateTestAccess::open_unmapped(
                     **delegate,
                     grab::CoordinateSpaceId{ 2U }
    )
                     .has_value() );
    EXPECT_NE( x11_detail::X11OverlayDelegateTestAccess::window( **delegate ),
               XCB_WINDOW_NONE );
    ( *delegate )->close();
    ( *delegate )->close();
}
