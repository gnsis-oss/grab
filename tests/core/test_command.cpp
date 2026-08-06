// PLACEHOLDER — registered in tests/CMakeLists.txt by Phase 0 so the contract
// unit can replace this file without touching a shared build file.

#include "grab/command.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <variant>
// clang-format on

namespace
{

    TEST( Placeholder,
          CommandVariantCompiles )
    {
        static_assert( std::variant_size_v<grab::sequence::Command> ==
                       grab::sequence::sequenceCommandCount );
        SUCCEED();
    }

}    // namespace
