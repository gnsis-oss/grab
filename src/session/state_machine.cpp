#include "grab/session.hpp"
#include "session/state_machine.hpp"

namespace grab::session
{

    bool
    is_valid_transition( SessionState from,
                         SessionState to ) noexcept
    {
        if( to == SessionState::failed )
        {
            return from ==
                   SessionState::starting ||
                   from ==
                   SessionState::ready ||
                   from == SessionState::draining;
        }

        switch( from )
        {
            case SessionState::starting :
                return to == SessionState::ready;
            case SessionState::ready :
                return to == SessionState::draining;
            case SessionState::draining :
                return to == SessionState::stopped;
            case SessionState::stopped :
            case SessionState::failed :
            case SessionState::count :
                return false;
        }
        return false;
    }

}    // namespace grab::session
