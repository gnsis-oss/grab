#pragma once

#include "session/provider.hpp"

#include <vector>

namespace grab::session
{

    // The compiled-in real session providers, best-effort for every platform.
    // Phase 1 ships only the X11 shared-seat provider; the Wayland providers are
    // added by later plans. The returned pointers reference process-lifetime
    // singletons and must not be freed.
    [[nodiscard]]
    std::vector<const SessionProvider*>
    builtin_session_providers();

}    // namespace grab::session
