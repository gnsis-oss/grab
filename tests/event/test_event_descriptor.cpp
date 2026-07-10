#include "grab/event_descriptor.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <optional>
#include <string_view>
// clang-format on

namespace
{

    constexpr auto             descriptorCount      = 22U;
    constexpr auto             keyDownKind          = grab::EventKind::KeyDown;
    constexpr auto             windowCreatedKind    = grab::EventKind::WindowCreated;
    constexpr auto             a11yTextChangedKind  = grab::EventKind::A11yTextChanged;
    constexpr auto             appContextUpdateKind = grab::EventKind::AppContextUpdate;
    constexpr auto             browserTabKind      = grab::EventKind::BrowserTabSwitched;
    constexpr auto             stateSnapshotKind   = grab::EventKind::StateSnapshot;
    constexpr auto             inputCategory       = grab::EventCategory::Input;
    constexpr auto             windowCategory      = grab::EventCategory::Window;
    constexpr auto             a11yCategory        = grab::EventCategory::Accessibility;
    constexpr auto             integrationCategory = grab::EventCategory::Integration;
    constexpr auto             browserCategory     = grab::EventCategory::Browser;
    constexpr auto             stateCategory       = grab::EventCategory::State;
    constexpr std::string_view browserTabSwitched  = "browser.tab_switched";
    constexpr std::string_view appContextUpdate    = "app.context_update";
    constexpr std::string_view appTabChanged       = "app.tab_changed";
    constexpr std::string_view inputKeyDown        = "input.key_down";
    constexpr std::string_view unspecified         = "unspecified";
    constexpr std::string_view flatTabSwitched     = "tab_switched";
    constexpr std::string_view flatContextUpdate   = "context_update";
    constexpr std::string_view unknownType         = "does.not.exist";

    static_assert( grab::detail::eventDescriptors.size() == descriptorCount );
    static_assert( grab::category_of( keyDownKind ) == inputCategory );
    static_assert( grab::category_of( windowCreatedKind ) == windowCategory );
    static_assert( grab::category_of( a11yTextChangedKind ) == a11yCategory );
    static_assert( grab::category_of( appContextUpdateKind ) == integrationCategory );
    static_assert( grab::category_of( browserTabKind ) == browserCategory );
    static_assert( grab::category_of( stateSnapshotKind ) == stateCategory );

}    // namespace

TEST( EventDescriptor,
      MapsKindToWireName )
{
    EXPECT_EQ( grab::wire_name( browserTabKind ), browserTabSwitched );
    EXPECT_EQ( grab::wire_name( appContextUpdateKind ), appContextUpdate );
    EXPECT_EQ( grab::wire_name( keyDownKind ), inputKeyDown );
}

TEST( EventDescriptor,
      MapsWireNameToKind )
{
    EXPECT_EQ( grab::wire_kind( browserTabSwitched ), browserTabKind );
    EXPECT_EQ( grab::wire_kind( appContextUpdate ), appContextUpdateKind );
    EXPECT_EQ( grab::wire_kind( appTabChanged ), grab::EventKind::AppTabChanged );
}

TEST( EventDescriptor,
      RejectsDroppedFlatAliases )
{
    EXPECT_EQ( grab::wire_kind( flatTabSwitched ), std::nullopt );
    EXPECT_EQ( grab::wire_kind( flatContextUpdate ), std::nullopt );
}

TEST( EventDescriptor,
      RejectsUnknownType )
{
    EXPECT_EQ( grab::wire_kind( unknownType ), std::nullopt );
    EXPECT_EQ( grab::wire_name( grab::EventKind::Unspecified ), unspecified );
}

TEST( EventDescriptor,
      RoundTripsEveryKind )
{
    for( const auto& descriptor : grab::detail::eventDescriptors )
    {
        EXPECT_EQ( grab::wire_kind( descriptor.wire_name ), descriptor.kind );
        EXPECT_EQ( grab::wire_name( descriptor.kind ), descriptor.wire_name );
    }
}
