// PLACEHOLDER — registered in tests/CMakeLists.txt by Phase 0 so the CLI unit
// can replace this file without touching a shared build file.
//
// The Play row is already in the descriptor table, so the verb resolves even
// though nothing runs it yet. Landing the row now is deliberate: adding it
// later would break an earlier unit's descriptor-count assertion in a file it
// does not own.

#include "frontends/cli/common.hpp"
#include "grab/command_descriptor.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <string_view>
// clang-format on

namespace
{

    constexpr std::string_view playVerb{ "play" };

    TEST( Placeholder,
          PlayVerbResolves )
    {
        const auto* const descriptor = grab::cli::find_command_by_verb( playVerb );
        ASSERT_NE( descriptor, nullptr );
        EXPECT_EQ( descriptor->kind, grab::CommandKind::Play );
    }

}    // namespace
