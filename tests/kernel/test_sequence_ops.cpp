// PLACEHOLDER — registered in tests/CMakeLists.txt by Phase 0 so the
// sequence-ops unit can replace this file without touching a shared build file.

#include "kernel/sequence/sequence_ops.hpp"

// clang-format off
#include <gtest/gtest.h>
// clang-format on

namespace
{

    TEST( Placeholder,
          SequenceOpsCompiles )
    {
        const grab::kernel::sequence::Sequence empty;
        // validate() is a declared pass-through seam, so it already answers.
        EXPECT_TRUE( grab::kernel::sequence::validate( empty ).has_value() );
    }

}    // namespace
