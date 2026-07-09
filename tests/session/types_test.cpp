#include "grab/session.hpp"

// clang-format off
#include <gtest/gtest.h>
// clang-format on

TEST( SessionTypes,
      ModeAndStateNamesRoundTrip )
{
    EXPECT_EQ( grab::mode_name( grab::SessionMode::offscreen ), "offscreen" );
    EXPECT_EQ( grab::mode_from_string( "shared" ), grab::SessionMode::shared );
    EXPECT_EQ( grab::state_name( grab::SessionState::ready ), "ready" );
    EXPECT_EQ( grab::session_state_from_string( "draining" ),
               grab::SessionState::draining );
    EXPECT_FALSE( grab::mode_from_string( "bogus" ).has_value() );
}
