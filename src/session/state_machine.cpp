#include "grab/session.hpp"
#include "session/state_machine.hpp"

namespace grab::session
{

    bool
    is_valid_transition( SessionState from,
                         SessionState to ) noexcept
    {
        if( to == SessionState::Failed )
        {
            return from ==
                   SessionState::Starting ||
                   from ==
                   SessionState::Ready ||
                   from == SessionState::Draining;
        }

        switch( from )
        {
            case SessionState::Starting :
                return to == SessionState::Ready;
            case SessionState::Ready :
                return to == SessionState::Draining;
            case SessionState::Draining :
                return to == SessionState::Stopped;
            case SessionState::Stopped :
            case SessionState::Failed :
            case SessionState::Count :
                return false;
        }
        return false;
    }

}    // namespace grab::session
