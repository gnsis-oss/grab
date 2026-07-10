#include "grab/event.hpp"
#include "grab/event_wire.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <optional>
#include <string_view>
// clang-format on

namespace
{

    constexpr std::string_view kBrowserTabSwitched = "browser.tab_switched";
    constexpr std::string_view kAppContextUpdate   = "app.context_update";
    constexpr std::string_view kAppTabChanged      = "app.tab_changed";
    constexpr std::string_view kInputKeyDown       = "input.key_down";
    constexpr std::string_view kUnspecified        = "unspecified";
    constexpr std::string_view kFlatTabSwitched    = "tab_switched";
    constexpr std::string_view kFlatContextUpdate  = "context_update";
    constexpr std::string_view kUnknownType        = "does.not.exist";

}    // namespace

TEST( EventWire,
      MapsKindToWireName )
{
    EXPECT_EQ( grab::wire_name( grab::EventKind::browser_tab_switched ),
               kBrowserTabSwitched );
    EXPECT_EQ( grab::wire_name( grab::EventKind::app_context_update ),
               kAppContextUpdate );
    EXPECT_EQ( grab::wire_name( grab::EventKind::key_down ), kInputKeyDown );
}

TEST( EventWire,
      MapsWireNameToKind )
{
    EXPECT_EQ( grab::wire_kind( kBrowserTabSwitched ),
               grab::EventKind::browser_tab_switched );
    EXPECT_EQ( grab::wire_kind( kAppContextUpdate ),
               grab::EventKind::app_context_update );
    EXPECT_EQ( grab::wire_kind( kAppTabChanged ), grab::EventKind::app_tab_changed );
}

TEST( EventWire,
      RejectsDroppedFlatAliases )
{
    EXPECT_EQ( grab::wire_kind( kFlatTabSwitched ), std::nullopt );
    EXPECT_EQ( grab::wire_kind( kFlatContextUpdate ), std::nullopt );
}

TEST( EventWire,
      RejectsUnknownType )
{
    EXPECT_EQ( grab::wire_kind( kUnknownType ), std::nullopt );
    EXPECT_EQ( grab::wire_name( grab::EventKind::unspecified ), kUnspecified );
}

TEST( EventWire,
      RoundTripsEveryKind )
{
    for( const auto& entry : grab::detail::kEventKindWireNames.entries )
    {
        EXPECT_EQ( grab::wire_kind( entry.text ), entry.value );
        EXPECT_EQ( grab::wire_name( entry.value ), entry.text );
    }
}
