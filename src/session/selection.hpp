#pragma once

#include "core/environment.hpp"
#include "grab/capability.hpp"
#include "grab/result.hpp"
#include "grab/session.hpp"
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
                             grab::SessionMode                       mode );

    struct SessionModeReport
    {
            grab::SessionMode       mode = grab::SessionMode::offscreen;
            std::string             provider;
            grab::AvailabilityState state = grab::AvailabilityState::unavailable;
            std::string             reason;
    };

    [[nodiscard]]
    std::vector<SessionModeReport>
    session_availability_report( std::span<const SessionProvider* const> providers,
                                 const grab::core::Environment&          env );

}    // namespace grab::session
