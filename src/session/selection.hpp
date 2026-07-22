#pragma once

#include "grab/capability.hpp"
#include "grab/result.hpp"
#include "grab/workspace.hpp"
#include "kernel/support/environment.hpp"
#include "session/provider.hpp"

#include <span>
#include <string>
#include <vector>

namespace grab::session
{

    [[nodiscard]]
    grab::Result<const SessionProvider*>
    select_session_provider( std::span<const SessionProvider* const> providers,
                             const grab::core::Environment&          env,
                             grab::WorkspaceMode                     mode );

    struct SessionModeReport
    {
            grab::WorkspaceMode     mode = grab::WorkspaceMode::Offscreen;
            std::string             provider;
            grab::AvailabilityState state = grab::AvailabilityState::Unavailable;
            std::string             reason;
    };

    [[nodiscard]]
    std::vector<SessionModeReport>
    session_availability_report( std::span<const SessionProvider* const> providers,
                                 const grab::core::Environment&          env );

}    // namespace grab::session
