#pragma once

#include "grab/session.hpp"

namespace grab::session
{

    [[nodiscard]]
    bool
    is_valid_transition( SessionState from,
                         SessionState to ) noexcept;

}    // namespace grab::session
