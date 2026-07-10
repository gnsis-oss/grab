#include "grab/event.hpp"
#include "grab/event_wire.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <optional>
#include <string_view>
// clang-format on

namespace
{

    constexpr std::string_view browserTabSwitched = "browser.tab_switched";
    constexpr std::string_view appContextUpdate   = "app.context_update";
    constexpr std::string_view appTabChanged      = "app.tab_changed";
    constexpr std::string_view inputKeyDown       = "input.key_down";
    constexpr std::string_view unspecified        = "unspecified";
    constexpr std::string_view flatTabSwitched    = "tab_switched";
    constexpr std::string_view flatContextUpdate  = "context_update";
    constexpr std::string_view unknownType        = "does.not.exist";

}    // namespace

TEST( EventWire,
      MapsKindToWireName )
{
    EXPECT_EQ( grab::wire_name( grab::EventKind::BrowserTabSwitched ),
               browserTabSwitched );
    EXPECT_EQ( grab::wire_name( grab::EventKind::AppContextUpdate ), appContextUpdate );
    EXPECT_EQ( grab::wire_name( grab::EventKind::KeyDown ), inputKeyDown );
}

TEST( EventWire,
      MapsWireNameToKind )
{
    EXPECT_EQ( grab::wire_kind( browserTabSwitched ),
               grab::EventKind::BrowserTabSwitched );
    EXPECT_EQ( grab::wire_kind( appContextUpdate ), grab::EventKind::AppContextUpdate );
    EXPECT_EQ( grab::wire_kind( appTabChanged ), grab::EventKind::AppTabChanged );
}

TEST( EventWire,
      RejectsDroppedFlatAliases )
{
    EXPECT_EQ( grab::wire_kind( flatTabSwitched ), std::nullopt );
    EXPECT_EQ( grab::wire_kind( flatContextUpdate ), std::nullopt );
}

TEST( EventWire,
      RejectsUnknownType )
{
    EXPECT_EQ( grab::wire_kind( unknownType ), std::nullopt );
    EXPECT_EQ( grab::wire_name( grab::EventKind::Unspecified ), unspecified );
}

TEST( EventWire,
      RoundTripsEveryKind )
{
    for( const auto& entry : grab::detail::eventKindWireNames.entries )
    {
        EXPECT_EQ( grab::wire_kind( entry.text ), entry.value );
        EXPECT_EQ( grab::wire_name( entry.value ), entry.text );
    }
}
