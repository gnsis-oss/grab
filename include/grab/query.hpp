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

    // The geometry, identity and state of a resolved node at describe() time.
    // `bounds` is the node's rectangle in its snapshot coordinate space;
    // `states` is the NodeState bitmask (see grab/ui.hpp); `facets` is the
    // Facet bitmask advertising object-scoped behavior (Text/Invokable/...);
    // `provenance` records the runtime and revision the record came from.
    //
    // The text-bearing fields are empty when the backend does not expose the
    // corresponding property: `name` is the accessible name (a link's anchor
    // label, a button's caption); `title` is the window/frame title; `text` is
    // the node's own textual content (a paragraph's prose); `url` is the link
    // target when the node is a hyperlink. A consumer distinguishes "absent"
    // from "present but empty" only via the underlying property read, so an
    // empty string here means "no value surfaced," never a hard error.
    struct NodeInfo
    {
            SpaceRect     bounds{};
            std::uint32_t states{};
            std::uint32_t facets{};
            RoleId        role{};
            std::string   name;
            std::string   title;
            std::string   text;
            std::string   url;
            UiProvenance  provenance{};
    };

}    // namespace grab
