#pragma once

#include "grab/workspace.hpp"

namespace grab::session
{

    [[nodiscard]]
    bool
    is_valid_transition( WorkspaceState from,
                         WorkspaceState to ) noexcept;

}    // namespace grab::session
