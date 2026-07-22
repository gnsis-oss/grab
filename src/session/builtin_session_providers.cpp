#include "session/builtin_session_providers.hpp"
#include "session/provider.hpp"
#include "session/x11_seat_provider.hpp"

#include <vector>

namespace grab::session
{

    std::vector<const SessionProvider*>
    builtin_session_providers()
    {
        static const X11SeatSessionProvider x11_seat;
        return std::vector<const SessionProvider*>{ &x11_seat };
    }

}    // namespace grab::session
