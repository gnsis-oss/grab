// PLACEHOLDER — registered in tests/CMakeLists.txt by Phase 0 so the contract
// unit can replace this file without touching a shared build file.

#include "grab/sequence_types.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstddef>
// clang-format on

namespace
{

    // 16-bit index with generation starting at 1, so index 0 is usable and the
    // capacity is one more than the naive reading of "16 bits".
    constexpr std::size_t expectedMaxSteps = 65'536U;

    TEST( Placeholder,
          SequenceTypesCompiles )
    {
        static_assert( grab::sequence::maxSteps == expectedMaxSteps );
        SUCCEED();
    }

}    // namespace
