#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/ids.hpp"
#include "grab/locator.hpp"
#include "grab/space.hpp"

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

}    // namespace grab
