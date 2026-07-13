#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/locator.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace grab::kernel::query::detail
{

    enum class LocatorOp : std::uint8_t
    {
        MatchAll,
        MatchNone,
        Role,
        State,
        Property,
        AccessibleName,
        Text,
        All,
        Any,
        Not,
        ChildOf,
        DescendantOf,
        AncestorOf,
        Related,
        RelatedReverse,
    };

    struct LocatorPlan;
    using LocatorPlanPtr = std::shared_ptr<const LocatorPlan>;

    struct LocatorPlan
    {
            LocatorOp                   op{ LocatorOp::MatchAll };
            RoleId                      role{};
            NodeState                   state{ static_cast<NodeState>( 0U ) };
            PropertyId                  property{};
            PropertyValue               value;
            std::string                 text;
            RelationId                  relation{};
            std::vector<LocatorPlanPtr> children;
    };

}    // namespace grab::kernel::query::detail
