// PLACEHOLDER — registered in tests/CMakeLists.txt by Phase 0 so the
// interpreter unit can replace this file without touching a shared build file.

#include "kernel/sequence/interpreter.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <string_view>
// clang-format on

namespace
{

    constexpr std::string_view emptyDocument{ "{}" };

    TEST( Placeholder,
          InterpreterCompiles )
    {
        // The Phase 0 stub declines everything; the interpreter unit replaces
        // it and this assertion with the real grammar tests.
        EXPECT_FALSE( grab::kernel::sequence::parse( emptyDocument ).has_value() );
    }

}    // namespace
