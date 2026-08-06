// PLACEHOLDER — registered in tests/CMakeLists.txt by Phase 0 so the contract
// unit can replace this file without touching a shared build file.

#include "grab/command_descriptor.hpp"

// clang-format off
#include <gtest/gtest.h>
// clang-format on

namespace
{

    TEST( Placeholder,
          CommandDescriptorExtensionCompiles )
    {
        static_assert( grab::timing_class_of( grab::CommandKind::Wait ) ==
                       grab::sequence::TimingClass::Timed );
        SUCCEED();
    }

}    // namespace
