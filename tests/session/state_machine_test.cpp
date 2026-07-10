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
    EXPECT_TRUE( is_valid_transition( SessionState::Starting, SessionState::Ready ) );
    EXPECT_TRUE( is_valid_transition( SessionState::Ready, SessionState::Draining ) );
    EXPECT_TRUE( is_valid_transition( SessionState::Draining, SessionState::Stopped ) );
}

TEST( SessionStateMachine,
      AllowsFailureFromAnyLiveState )
{
    EXPECT_TRUE( is_valid_transition( SessionState::Starting, SessionState::Failed ) );
    EXPECT_TRUE( is_valid_transition( SessionState::Ready, SessionState::Failed ) );
    EXPECT_TRUE( is_valid_transition( SessionState::Draining, SessionState::Failed ) );
}

TEST( SessionStateMachine,
      RejectsIllegalTransitions )
{
    EXPECT_FALSE( is_valid_transition( SessionState::Stopped, SessionState::Ready ) );
    EXPECT_FALSE( is_valid_transition( SessionState::Ready, SessionState::Starting ) );
    EXPECT_FALSE( is_valid_transition( SessionState::Ready, SessionState::Ready ) );
}
