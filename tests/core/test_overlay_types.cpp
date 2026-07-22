#include "grab/overlay.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstddef>
#include <cstdint>
#include <variant>
// clang-format on

namespace
{

    constexpr std::size_t            lifetimeAlternativeCount    = 3U;
    constexpr std::size_t            pathCommandAlternativeCount = 4U;
    constexpr std::size_t            geometryAlternativeCount    = 4U;
    constexpr std::size_t            sceneChangeAlternativeCount = 3U;
    constexpr std::uint8_t           opaqueAlpha                 = 255U;
    constexpr std::uint64_t          earlierEpochValue           = 1U;
    constexpr std::uint64_t          laterEpochValue             = 2U;
    constexpr std::uint32_t          earlierSlotValue            = 3U;
    constexpr std::uint32_t          laterSlotValue              = 4U;

    constexpr grab::overlay::ShapeId earlierEpochShapeId{
        .epoch = { .value = earlierEpochValue },
        .slot  = laterSlotValue,
    };
    constexpr grab::overlay::ShapeId laterEpochShapeId{
        .epoch = { .value = laterEpochValue },
        .slot  = earlierSlotValue,
    };
    constexpr grab::overlay::ShapeId earlierSlotShapeId{
        .epoch = { .value = laterEpochValue },
        .slot  = earlierSlotValue,
    };
    constexpr grab::overlay::ShapeId laterSlotShapeId{
        .epoch = { .value = laterEpochValue },
        .slot  = laterSlotValue,
    };

}    // namespace

TEST( OverlayTypes,
      VariantsHaveClosedAlternativeCounts )
{
    using SceneChange = decltype( grab::overlay::SceneDelta{}.change );

    static_assert( std::variant_size_v<grab::overlay::LifetimePolicy> ==
                   lifetimeAlternativeCount );
    static_assert( std::variant_size_v<grab::overlay::PathCommand> ==
                   pathCommandAlternativeCount );
    static_assert( std::variant_size_v<grab::overlay::Geometry> ==
                   geometryAlternativeCount );
    static_assert( std::variant_size_v<SceneChange> == sceneChangeAlternativeCount );
}

TEST( OverlayTypes,
      ShapeIdOrdersByEpochThenSlot )
{
    static_assert( earlierEpochShapeId < laterEpochShapeId );
    static_assert( earlierSlotShapeId < laterSlotShapeId );
}

TEST( OverlayTypes,
      ColorDefaultsToOpaqueAlpha )
{
    static_assert( grab::overlay::Color{}.a == opaqueAlpha );
}

TEST( OverlayTypes,
      SceneDeltaHoldsEveryChangeAlternative )
{
    constexpr grab::overlay::SceneDelta upsertDelta{ .change = grab::overlay::Upsert{} };
    constexpr grab::overlay::SceneDelta removeDelta{ .change = grab::overlay::Remove{} };
    constexpr grab::overlay::SceneDelta clearDelta{ .change = grab::overlay::Clear{} };

    static_assert( std::holds_alternative<grab::overlay::Upsert>( upsertDelta.change ) );
    static_assert( std::holds_alternative<grab::overlay::Remove>( removeDelta.change ) );
    static_assert( std::holds_alternative<grab::overlay::Clear>( clearDelta.change ) );
}
