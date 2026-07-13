#include "grab/command_descriptor.hpp"

#include <array>
#include <cstddef>
#include <gtest/gtest.h>
#include <string_view>

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
        "screen.batch",
        "image.compare",
        "screen.watch",
        "input.key",
        "session.open",
    } );

}    // namespace

TEST( CommandDescriptor,
      ListCommandsReturnsCanonicalSet )
{
    const auto& commands = grab::list_commands();
    ASSERT_EQ( commands.size(), expectedCommandNames.size() );

    for( std::size_t index = 0U; index < commands.size(); ++index )
    {
        EXPECT_EQ( commands[index].name, expectedCommandNames[index] );
    }
}

TEST( CommandDescriptor,
      NamesAreUniqueAndRoundTrip )
{
    const auto& commands = grab::list_commands();
    for( std::size_t left = 0U; left < commands.size(); ++left )
    {
        const auto kind = grab::command_kind( commands[left].name );
        ASSERT_TRUE( kind.has_value() );
        EXPECT_EQ( *kind, commands[left].kind );
        EXPECT_EQ( grab::command_name( commands[left].kind ), commands[left].name );

        for( std::size_t right = left + 1U; right < commands.size(); ++right )
        {
            EXPECT_NE( commands[left].name, commands[right].name );
        }
    }
}
