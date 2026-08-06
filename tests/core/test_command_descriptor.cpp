#include "grab/command_descriptor.hpp"
#include "grab/trace.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <set>
#include <string_view>
// clang-format on

namespace
{

    constexpr auto expectedCommandNames = std::to_array<std::string_view>( {
        "system.doctor",
        "service.daemon",
        "input.type",
        "input.click",
        "input.drag",
        "input.drag_curve",
        "screen.capture",
        "screen.windows",
        "window.focus",
        "window.place",
        "screen.batch",
        "image.compare",
        "screen.watch",
        "input.key",
        "session.open",
        "overlay.trail",
        "overlay.shape",
        "overlay.feedback",
        "overlay.sketch",
        "input.move",
        "input.warp",
        "input.follow",
        "input.press",
        "input.release",
        "input.scroll",
        "input.click_at",
        "input.key_down",
        "input.key_up",
        "time.wait",
        "system.play",
    } );

}    // namespace

TEST( CommandDescriptor,
      ListCommandsReturnsCanonicalSet )
{
    const auto& commands = grab::list_commands();
    ASSERT_EQ( commands.size(), expectedCommandNames.size() );

    EXPECT_TRUE( std::ranges::equal( commands,
                                     expectedCommandNames,
                                     {},
                                     &grab::CommandDescriptor::name ) );
}

TEST( CommandDescriptor,
      OverlayRowsCarryInProcessMutationMetadata )
{
    constexpr std::array overlayNames{
        std::string_view{ "overlay.trail" },
        std::string_view{ "overlay.shape" },
        std::string_view{ "overlay.feedback" },
        std::string_view{ "overlay.sketch" },
    };

    for( const auto name : overlayNames )
    {
        const auto&       commands = grab::list_commands();
        const auto* const found =
            std::ranges::find( commands, name, &grab::CommandDescriptor::name );
        ASSERT_NE( found, commands.end() ) << name;
        EXPECT_EQ( found->retry, grab::RetryClass::ResolveOnly );
        EXPECT_EQ( found->mutability, grab::Mutability::Mutating );
        EXPECT_FALSE( found->idempotent );
        EXPECT_FALSE( found->consent_gated );
    }
}

TEST( CommandDescriptor,
      NamesAreUniqueAndRoundTrip )
{
    const auto&                commands = grab::list_commands();
    std::set<std::string_view> names;
    for( const auto& command : commands )
    {
        EXPECT_TRUE( names.insert( command.name ).second ) << command.name;
        const auto kind = grab::command_kind( command.name );
        ASSERT_TRUE( kind.has_value() );
        EXPECT_EQ( *kind, command.kind );
        EXPECT_EQ( grab::command_name( command.kind ), command.name );
    }
}
