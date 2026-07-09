#include "grab/session.hpp"
#include "session/state_machine.hpp"

// clang-format off
#include <gtest/gtest.h>
// clang-format on

namespace
{

    using grab::SessionState;
    using grab::session::is_valid_transition;

}    // namespace

TEST( SessionStateMachine,
      AllowsForwardProgress )
{
    EXPECT_TRUE( is_valid_transition( SessionState::starting, SessionState::ready ) );
    EXPECT_TRUE( is_valid_transition( SessionState::ready, SessionState::draining ) );
    EXPECT_TRUE( is_valid_transition( SessionState::draining, SessionState::stopped ) );
}

TEST( SessionStateMachine,
      AllowsFailureFromAnyLiveState )
{
    EXPECT_TRUE( is_valid_transition( SessionState::starting, SessionState::failed ) );
    EXPECT_TRUE( is_valid_transition( SessionState::ready, SessionState::failed ) );
    EXPECT_TRUE( is_valid_transition( SessionState::draining, SessionState::failed ) );
}

TEST( SessionStateMachine,
      RejectsIllegalTransitions )
{
    EXPECT_FALSE( is_valid_transition( SessionState::stopped, SessionState::ready ) );
    EXPECT_FALSE( is_valid_transition( SessionState::ready, SessionState::starting ) );
    EXPECT_FALSE( is_valid_transition( SessionState::ready, SessionState::ready ) );
}
