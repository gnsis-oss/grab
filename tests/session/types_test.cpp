#include "grab/workspace.hpp"

// clang-format off
#include <gtest/gtest.h>
// clang-format on

TEST( SessionTypes,
      ModeAndStateNamesRoundTrip )
{
    EXPECT_EQ( grab::mode_name( grab::WorkspaceMode::Offscreen ), "offscreen" );
    EXPECT_EQ( grab::mode_from_string( "shared" ), grab::WorkspaceMode::Shared );
    EXPECT_EQ( grab::state_name( grab::WorkspaceState::Ready ), "ready" );
    EXPECT_EQ( grab::session_state_from_string( "draining" ),
               grab::WorkspaceState::Draining );
    EXPECT_FALSE( grab::mode_from_string( "bogus" ).has_value() );
}
