#include "grab/capability.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstddef>
#include <string_view>
// clang-format on

namespace
{

    constexpr std::string_view kScreenDisplayImageId      = "screen.display.image";
    constexpr std::string_view kScreenDisplayVideoId      = "screen.display.video";
    constexpr std::string_view kScreenWindowImageId       = "screen.window.image";
    constexpr std::string_view kScreenActiveWindowImageId = "screen.active_window.image";
    constexpr std::string_view kScreenUserSelectedImageId = "screen.user_selected.image";
    constexpr std::string_view kEventKeyGlobalId          = "event.key.global";
    constexpr std::string_view kEventMouseGlobalId        = "event.mouse.global";
    constexpr std::string_view kEventWindowFocusId        = "event.window.focus";
    constexpr std::string_view kEventWindowListId         = "event.window.list";
    constexpr std::string_view kWindowFindId              = "window.find";
    constexpr std::string_view kWindowGeometryId          = "window.geometry";
    constexpr std::string_view kEventWidgetPrefixId       = "event.widget";
    constexpr std::string_view kMouseMoveId               = "mouse.move";
    constexpr std::string_view kMouseMoveAbsoluteId       = "mouse.move.absolute";
    constexpr std::string_view kMouseMoveRelativeId       = "mouse.move.relative";
    constexpr std::string_view kMouseClickId              = "mouse.click";
    constexpr std::string_view kMouseDragId               = "mouse.drag";
    constexpr std::string_view kKeyTextId                 = "key.text";
    constexpr std::string_view kKeyChordId                = "key.chord";
    constexpr std::string_view kAvailableStateName        = "available";
    constexpr std::string_view kDegradedStateName         = "degraded";
    constexpr std::string_view kNeedsPermissionName       = "needs-permission";
    constexpr std::string_view kUnavailableStateName      = "unavailable";
    constexpr auto             kAvailableState = grab::AvailabilityState::available;
    constexpr auto             kDegradedState  = grab::AvailabilityState::degraded;
    constexpr auto kNeedsPermissionState   = grab::AvailabilityState::needs_permission;
    constexpr auto kUnavailableState       = grab::AvailabilityState::unavailable;
    constexpr auto kScreenWindowCapability = grab::Capability::screen_window_image;
    constexpr auto kMouseMoveCapability    = grab::Capability::mouse_move;
    constexpr auto kKeyChordCapability     = grab::Capability::key_chord;
    constexpr auto kConcreteCapabilityCount =
        static_cast<std::size_t>( grab::Capability::count );

}    // namespace

TEST( Capability,
      IdsMatchSpecTaxonomy )
{
    EXPECT_EQ( grab::capability::screen_display_image, kScreenDisplayImageId );
    EXPECT_EQ( grab::capability::screen_display_video, kScreenDisplayVideoId );
    EXPECT_EQ( grab::capability::screen_window_image, kScreenWindowImageId );
    EXPECT_EQ( grab::capability::screen_active_window_image,
               kScreenActiveWindowImageId );
    EXPECT_EQ( grab::capability::screen_user_selected_image,
               kScreenUserSelectedImageId );
    EXPECT_EQ( grab::capability::event_key_global, kEventKeyGlobalId );
    EXPECT_EQ( grab::capability::event_mouse_global, kEventMouseGlobalId );
    EXPECT_EQ( grab::capability::event_window_focus, kEventWindowFocusId );
    EXPECT_EQ( grab::capability::event_window_list, kEventWindowListId );
    EXPECT_EQ( grab::capability::window_find, kWindowFindId );
    EXPECT_EQ( grab::capability::window_geometry, kWindowGeometryId );
    EXPECT_EQ( grab::capability::event_widget_prefix, kEventWidgetPrefixId );
    EXPECT_EQ( grab::capability::mouse_move, kMouseMoveId );
    EXPECT_EQ( grab::capability::mouse_move_absolute, kMouseMoveAbsoluteId );
    EXPECT_EQ( grab::capability::mouse_move_relative, kMouseMoveRelativeId );
    EXPECT_EQ( grab::capability::mouse_click, kMouseClickId );
    EXPECT_EQ( grab::capability::mouse_drag, kMouseDragId );
    EXPECT_EQ( grab::capability::key_text, kKeyTextId );
    EXPECT_EQ( grab::capability::key_chord, kKeyChordId );
}

TEST( Capability,
      StateNamesAreStable )
{
    EXPECT_EQ( grab::state_name( kAvailableState ), kAvailableStateName );
    EXPECT_EQ( grab::state_name( kDegradedState ), kDegradedStateName );
    EXPECT_EQ( grab::state_name( kNeedsPermissionState ), kNeedsPermissionName );
    EXPECT_EQ( grab::state_name( kUnavailableState ), kUnavailableStateName );
}

TEST( Capability,
      EnumMapsToStableIds )
{
    static_assert( grab::capability_entries().size() == kConcreteCapabilityCount );

    EXPECT_EQ( grab::capability_name( kScreenWindowCapability ), kScreenWindowImageId );
    EXPECT_EQ( grab::capability_name( kMouseMoveCapability ), kMouseMoveId );
    EXPECT_EQ( grab::capability_name( kKeyChordCapability ), kKeyChordId );
}

TEST( Capability,
      StableIdsMapBackToEnum )
{
    const auto screen = grab::capability_from_string( kScreenWindowImageId );
    ASSERT_TRUE( screen.has_value() );
    EXPECT_EQ( *screen, kScreenWindowCapability );

    const auto chord = grab::capability_from_string( kKeyChordId );
    ASSERT_TRUE( chord.has_value() );
    EXPECT_EQ( *chord, kKeyChordCapability );

    EXPECT_FALSE( grab::capability_from_string( kEventWidgetPrefixId ).has_value() );
}
