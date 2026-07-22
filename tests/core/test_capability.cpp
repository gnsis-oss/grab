#include "grab/capability.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstddef>
#include <string_view>
// clang-format on

namespace
{

    constexpr std::string_view screenDisplayImageId      = "screen.display.image";
    constexpr std::string_view screenDisplayVideoId      = "screen.display.video";
    constexpr std::string_view screenWindowImageId       = "screen.window.image";
    constexpr std::string_view screenActiveWindowImageId = "screen.active_window.image";
    constexpr std::string_view screenUserSelectedImageId = "screen.user_selected.image";
    constexpr std::string_view eventKeyGlobalId          = "event.key.global";
    constexpr std::string_view eventMouseGlobalId        = "event.mouse.global";
    constexpr std::string_view eventWindowFocusId        = "event.window.focus";
    constexpr std::string_view eventWindowListId         = "event.window.list";
    constexpr std::string_view windowFindId              = "window.find";
    constexpr std::string_view windowGeometryId          = "window.geometry";
    constexpr std::string_view eventWidgetPrefixId       = "event.widget";
    constexpr std::string_view mouseMoveId               = "mouse.move";
    constexpr std::string_view mouseMoveAbsoluteId       = "mouse.move.absolute";
    constexpr std::string_view mouseMoveRelativeId       = "mouse.move.relative";
    constexpr std::string_view mouseClickId              = "mouse.click";
    constexpr std::string_view mouseDragId               = "mouse.drag";
    constexpr std::string_view keyTextId                 = "key.text";
    constexpr std::string_view keyChordId                = "key.chord";
    constexpr std::string_view availableStateName        = "available";
    constexpr std::string_view degradedStateName         = "degraded";
    constexpr std::string_view needsPermissionName       = "needs-permission";
    constexpr std::string_view unavailableStateName      = "unavailable";
    constexpr auto             availableState = grab::AvailabilityState::Available;
    constexpr auto             degradedState  = grab::AvailabilityState::Degraded;
    constexpr auto needsPermissionState       = grab::AvailabilityState::NeedsPermission;
    constexpr auto unavailableState           = grab::AvailabilityState::Unavailable;
    constexpr auto screenWindowCapability     = grab::Capability::ScreenWindowImage;
    constexpr auto mouseMoveCapability        = grab::Capability::MouseMove;
    constexpr auto keyChordCapability         = grab::Capability::KeyChord;
    constexpr auto concreteCapabilityCount =
        static_cast<std::size_t>( grab::Capability::Count );

}    // namespace

TEST( Capability,
      IdsMatchSpecTaxonomy )
{
    EXPECT_EQ( grab::capability::screen_display_image, screenDisplayImageId );
    EXPECT_EQ( grab::capability::screen_display_video, screenDisplayVideoId );
    EXPECT_EQ( grab::capability::screen_window_image, screenWindowImageId );
    EXPECT_EQ( grab::capability::screen_active_window_image, screenActiveWindowImageId );
    EXPECT_EQ( grab::capability::screen_user_selected_image, screenUserSelectedImageId );
    EXPECT_EQ( grab::capability::event_key_global, eventKeyGlobalId );
    EXPECT_EQ( grab::capability::event_mouse_global, eventMouseGlobalId );
    EXPECT_EQ( grab::capability::event_window_focus, eventWindowFocusId );
    EXPECT_EQ( grab::capability::event_window_list, eventWindowListId );
    EXPECT_EQ( grab::capability::window_find, windowFindId );
    EXPECT_EQ( grab::capability::window_geometry, windowGeometryId );
    EXPECT_EQ( grab::capability::event_widget_prefix, eventWidgetPrefixId );
    EXPECT_EQ( grab::capability::mouse_move, mouseMoveId );
    EXPECT_EQ( grab::capability::mouse_move_absolute, mouseMoveAbsoluteId );
    EXPECT_EQ( grab::capability::mouse_move_relative, mouseMoveRelativeId );
    EXPECT_EQ( grab::capability::mouse_click, mouseClickId );
    EXPECT_EQ( grab::capability::mouse_drag, mouseDragId );
    EXPECT_EQ( grab::capability::key_text, keyTextId );
    EXPECT_EQ( grab::capability::key_chord, keyChordId );
}

TEST( Capability,
      StateNamesAreStable )
{
    EXPECT_EQ( grab::state_name( availableState ), availableStateName );
    EXPECT_EQ( grab::state_name( degradedState ), degradedStateName );
    EXPECT_EQ( grab::state_name( needsPermissionState ), needsPermissionName );
    EXPECT_EQ( grab::state_name( unavailableState ), unavailableStateName );
}

TEST( Capability,
      EnumMapsToStableIds )
{
    static_assert( grab::capability_entries().size() == concreteCapabilityCount );

    EXPECT_EQ( grab::capability_name( screenWindowCapability ), screenWindowImageId );
    EXPECT_EQ( grab::capability_name( mouseMoveCapability ), mouseMoveId );
    EXPECT_EQ( grab::capability_name( keyChordCapability ), keyChordId );
}

TEST( Capability,
      StableIdsMapBackToEnum )
{
    const auto screen = grab::capability_from_string( screenWindowImageId );
    ASSERT_TRUE( screen.has_value() );
    EXPECT_EQ( *screen, screenWindowCapability );

    const auto chord = grab::capability_from_string( keyChordId );
    ASSERT_TRUE( chord.has_value() );
    EXPECT_EQ( *chord, keyChordCapability );

    EXPECT_FALSE( grab::capability_from_string( eventWidgetPrefixId ).has_value() );
}
