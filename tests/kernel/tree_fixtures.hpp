#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/ids.hpp"
#include "grab/role.hpp"
#include "grab/ui.hpp"
#include "spi/tree_source.hpp"

#include <cstdint>
#include <utility>
#include <vector>

// NOLINTBEGIN(readability-trailing-comma)
namespace grab::testing::tree
{

    inline constexpr RuntimeId     fixtureRuntime{ 7U };
    inline constexpr TreeEpoch     fixtureEpoch{ 2U };
    inline constexpr std::uint32_t fixtureTree{ 3U };

    [[nodiscard]]
    inline UiNodeRecord
    node( std::uint64_t id,
          RoleId        role     = grab::role::region,
          std::uint32_t states   = 0U,
          std::uint64_t revision = 1U )
    {
        return UiNodeRecord{
            NodeId{    id                    },
            NodeGeneration{                       1U },
            role,
            states,
            {                         },
            UiProvenance{
                   .runtime  = fixtureRuntime,.revision = revision,
                   },
        };
    }

    [[nodiscard]]
    inline UiSnapshot
    snapshot( std::uint64_t             revision,
              std::vector<UiNodeRecord> nodes,
              std::vector<UiRelation>   relations = {} )
    {
        return UiSnapshot::from_records(
            UiSnapshotMetadata{
                .runtime  = fixtureRuntime,
                .tree     = fixtureTree,
                .epoch    = fixtureEpoch,
                .revision = revision,
                .complete = true,
            },
            std::move( nodes ),
            {},
            std::move( relations )
        );
    }

    [[nodiscard]]
    inline spi::UiUpdate
    update( std::uint64_t sequence,
            UiSnapshot    value )
    {
        return spi::UiUpdate{
            .source_sequence = sequence,
            .payload         = std::move( value ),
        };
    }

}    // namespace grab::testing::tree

// NOLINTEND(readability-trailing-comma)
