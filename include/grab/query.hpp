#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/ids.hpp"
#include "grab/locator.hpp"
#include "grab/role.hpp"
#include "grab/space.hpp"
#include "grab/ui.hpp"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace grab
{

    using NodeSet    = std::vector<WidgetRef>;

    using QueryValue = std::
        variant<NodeSet, WidgetRef, std::string, double, bool, SpaceRect, SpacePoint>;

    struct ProviderProvenance
    {
            std::string   provider;
            std::string   candidate_provider;
            RuntimeId     runtime{};
            std::uint64_t revision{};
    };

    struct Match
    {
            WidgetRef                ref{};
            ConsistencyMode          mode{ ConsistencyMode::Live };
            std::uint64_t            snapshot_revision{};
            std::vector<std::string> matched_predicates;
            ProviderProvenance       provenance{};
    };

    // The geometry and state of a resolved node at describe() time. `bounds`
    // is the node's rectangle in its snapshot coordinate space; `states` is
    // the NodeState bitmask (see grab/ui.hpp); `provenance` records the
    // runtime and revision the record came from.
    struct NodeInfo
    {
            SpaceRect     bounds{};
            std::uint32_t states{};
            RoleId        role{};
            UiProvenance  provenance{};
    };

}    // namespace grab
