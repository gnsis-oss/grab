#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/locator.hpp"
#include "grab/query.hpp"
#include "grab/result.hpp"
#include "grab/ui.hpp"
#include "kernel/query/tree_nav.hpp"

#include <optional>
#include <string>
#include <vector>

namespace grab::kernel::query
{

    struct LoweredCandidates
    {
            std::vector<NodeId> nodes;
            std::string         provider;
    };

    // Providers may cheaply narrow a plan to candidates. The evaluator always
    // applies the complete locator locally afterward, preserving exact semantics.
    // A lowerer must return a sound superset of every node that can match.
    class PlanLowerer
    {
        public:

            PlanLowerer()                     = default;
            virtual ~PlanLowerer()            = default;
            PlanLowerer( const PlanLowerer& ) = delete;
            PlanLowerer&
            operator=( const PlanLowerer& ) = delete;
            PlanLowerer( PlanLowerer&& )    = delete;
            PlanLowerer&
            operator=( PlanLowerer&& ) = delete;

            [[nodiscard]]
            virtual Result<std::optional<LoweredCandidates>>
            lower( const Locator& locator,
                   const TreeNav& navigation ) const = 0;
    };

    struct QueryScope
    {
            // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
            const TreeNav&        navigation;
            // NOLINTNEXTLINE(readability-redundant-member-init)
            std::optional<NodeId> root{};
            const PlanLowerer*    lowerer{};
    };

    [[nodiscard]]
    Result<NodeSet>
    resolve_all( const Locator& locator,
                 QueryScope     scope,
                 LocatorLimits  limits = {} );

    [[nodiscard]]
    Result<Match>
    resolve( const Locator& locator,
             Cardinality    cardinality,
             QueryScope     scope,
             LocatorLimits  limits = {} );

}    // namespace grab::kernel::query
