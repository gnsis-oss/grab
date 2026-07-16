#include "cli/common.hpp"
#include "grab/command_descriptor.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <array>
#include <set>
#include <string>
#include <string_view>
// clang-format on

namespace
{

    constexpr std::array<std::string_view, 12U> expectedVerbs{
        "doctor",
        "daemon",
        "type",
        "click",
        "drag",
        "drag-curve",
        "capture",
        "batch",
        "compare",
        "watch",
        "key",
        "session",
    };
    constexpr std::string_view unknownVerb{ "frobnicate" };

    TEST( CommandTable,
          EveryDescriptorDerivesAUniqueCliVerb )
    {
        std::set<std::string> seen;
        for( const auto& descriptor : grab::list_commands() )
        {
            const auto verb = grab::cli::command_verb( descriptor );
            EXPECT_TRUE( seen.insert( verb ).second ) << verb;
            const auto* const found = grab::cli::find_command_by_verb( verb );
            ASSERT_NE( found, nullptr ) << verb;
            EXPECT_EQ( found->kind, descriptor.kind ) << verb;
        }
        EXPECT_EQ( seen.size(), grab::list_commands().size() );
    }

    TEST( CommandTable,
          EveryCliVerbMapsToADescriptor )
    {
        for( const auto verb : expectedVerbs )
        {
            EXPECT_NE( grab::cli::find_command_by_verb( verb ), nullptr ) << verb;
        }
        EXPECT_EQ( grab::cli::find_command_by_verb( unknownVerb ), nullptr );
    }

}    // namespace
