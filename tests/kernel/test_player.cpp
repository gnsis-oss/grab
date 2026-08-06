// PLACEHOLDER — registered in tests/CMakeLists.txt by Phase 0 so the player
// unit can replace this file without touching a shared build file.

#include "grab/sequence_types.hpp"
#include "kernel/sequence/player.hpp"
#include "kernel/sequence/sequence.hpp"

// clang-format off
#include <gtest/gtest.h>
// clang-format on

namespace
{

    TEST( Placeholder,
          PlayerCompiles )
    {
        const grab::kernel::sequence::Sequence empty;
        const grab::kernel::sequence::Player   player{ empty };
        EXPECT_EQ( player.state(), grab::sequence::PlayState::Idle );
        EXPECT_TRUE( player.frontier().empty() );
    }

}    // namespace
