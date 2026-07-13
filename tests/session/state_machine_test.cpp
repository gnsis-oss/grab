#include "grab/workspace.hpp"
#include "session/state_machine.hpp"

// clang-format off
#include <gtest/gtest.h>
// clang-format on

namespace
{

    using grab::WorkspaceState;
    using grab::session::is_valid_transition;

}    // namespace

TEST( SessionStateMachine,
      AllowsForwardProgress )
{
    EXPECT_TRUE( is_valid_transition( WorkspaceState::Starting,
                                      WorkspaceState::Ready ) );
    EXPECT_TRUE( is_valid_transition( WorkspaceState::Ready,
                                      WorkspaceState::Draining ) );
    EXPECT_TRUE( is_valid_transition( WorkspaceState::Draining,
                                      WorkspaceState::Stopped ) );
}

TEST( SessionStateMachine,
      AllowsFailureFromAnyLiveState )
{
    EXPECT_TRUE( is_valid_transition( WorkspaceState::Starting,
                                      WorkspaceState::Failed ) );
    EXPECT_TRUE( is_valid_transition( WorkspaceState::Ready, WorkspaceState::Failed ) );
    EXPECT_TRUE( is_valid_transition( WorkspaceState::Draining,
                                      WorkspaceState::Failed ) );
}

TEST( SessionStateMachine,
      RejectsIllegalTransitions )
{
    EXPECT_FALSE( is_valid_transition( WorkspaceState::Stopped,
                                       WorkspaceState::Ready ) );
    EXPECT_FALSE( is_valid_transition( WorkspaceState::Ready,
                                       WorkspaceState::Starting ) );
    EXPECT_FALSE( is_valid_transition( WorkspaceState::Ready, WorkspaceState::Ready ) );
}
