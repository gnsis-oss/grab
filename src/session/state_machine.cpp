#include "grab/workspace.hpp"
#include "session/state_machine.hpp"

namespace grab::session
{

    bool
    is_valid_transition( WorkspaceState from,
                         WorkspaceState to ) noexcept
    {
        if( to == WorkspaceState::Failed )
        {
            return from ==
                   WorkspaceState::Starting ||
                   from ==
                   WorkspaceState::Ready ||
                   from == WorkspaceState::Draining;
        }

        switch( from )
        {
            case WorkspaceState::Starting :
                return to == WorkspaceState::Ready;
            case WorkspaceState::Ready :
                return to == WorkspaceState::Draining;
            case WorkspaceState::Draining :
                return to == WorkspaceState::Stopped;
            case WorkspaceState::Stopped :
            case WorkspaceState::Failed :
            case WorkspaceState::Count :
                return false;
        }
        return false;
    }

}    // namespace grab::session
