#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/ids.hpp"
#include "grab/space.hpp"

namespace grab::input
{

    struct LocatedWindow
    {
            grab::WindowRef      window{};
            grab::SpaceRect      bounds{};
            grab::TransformTrust trust{ grab::TransformTrust::Untrusted };
    };

}    // namespace grab::input
