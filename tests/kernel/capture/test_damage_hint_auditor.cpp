#include "grab/geometry/rectangle.hpp"
#include "kernel/capture/damage_hint_auditor.hpp"
#include "kernel/capture/tile_differ.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <array>
#include <span>
#include <vector>
// clang-format on

namespace
{

    [[nodiscard]]
    grab::kernel::TileDiffResult
    four_dirty_tiles()
    {
        return grab::kernel::TileDiffResult{
            .kind = grab::kernel::TileDiffKind::DirtyTiles,
            .dirty_tiles =
                {
                              { .x = 0, .y = 0, .width = 8U, .height = 8U },
                              { .x = 8, .y = 0, .width = 8U, .height = 8U },
                              { .x = 16, .y = 0, .width = 8U, .height = 8U },
                              { .x = 24, .y = 0, .width = 8U, .height = 8U },
                              },
            .reason = {},
        };
    }

}    // namespace

TEST( DamageHintAuditor,
      DemotesSourceAboveMissThreshold )
{
    grab::kernel::DamageHintAuditor auditor{ 0.20 };
    const std::array                hints{
        grab::geometry::Rectangle{ .x = 1, .y = 1, .width = 1U, .height = 1U},
        grab::geometry::Rectangle{ .x = 9, .y = 1, .width = 1U, .height = 1U},
        grab::geometry::Rectangle{.x = 17, .y = 1, .width = 1U, .height = 1U},
    };

    const auto result = auditor.audit( std::span{ hints }, four_dirty_tiles() );

    EXPECT_EQ( result.action, grab::kernel::DamageHintAction::FullScan );
    EXPECT_DOUBLE_EQ( result.miss_rate, 0.25 );
    EXPECT_TRUE( auditor.degraded() );
    EXPECT_FALSE( auditor.degradation_reason().empty() );
}

TEST( DamageHintAuditor,
      AcceptsAccurateHintSource )
{
    grab::kernel::DamageHintAuditor auditor;
    const std::array                hints{
        grab::geometry::Rectangle{ .x = 0, .y = 0, .width = 32U, .height = 8U },
    };

    const auto result = auditor.audit( std::span{ hints }, four_dirty_tiles() );

    EXPECT_EQ( result.action, grab::kernel::DamageHintAction::UseHints );
    EXPECT_DOUBLE_EQ( result.miss_rate, 0.0 );
    EXPECT_FALSE( auditor.degraded() );
}

TEST( DamageHintAuditor,
      OverflowDemotesAndRequestsFullInvalidation )
{
    grab::kernel::DamageHintAuditor              auditor;
    const std::vector<grab::geometry::Rectangle> no_hints;

    const auto                                   result =
        auditor.audit( std::span<const grab::geometry::Rectangle>{ no_hints },
                       four_dirty_tiles(),
                       true );

    EXPECT_EQ( result.action, grab::kernel::DamageHintAction::FullInvalidation );
    EXPECT_TRUE( auditor.degraded() );
    EXPECT_FALSE( result.reason.empty() );
}
