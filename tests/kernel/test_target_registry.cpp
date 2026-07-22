#include "grab/ids.hpp"
#include "grab/presentation.hpp"
#include "grab/result.hpp"
#include "grab/space.hpp"
#include "kernel/graph/target_registry.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
// clang-format on

namespace
{

    constexpr std::uint32_t   processId = 4'242U;
    constexpr grab::SurfaceId surfaceId{ 17U };
    constexpr grab::SpaceRect bounds{
        .x     = 10.0,
        .y     = 20.0,
        .w     = 800.0,
        .h     = 600.0,
        .space = grab::CoordinateSpaceId{ 3U },
    };
    constexpr grab::SpaceRect updatedBounds{
        .x     = 30.0,
        .y     = 40.0,
        .w     = 1'024.0,
        .h     = 768.0,
        .space = grab::CoordinateSpaceId{ 4U },
    };

    [[nodiscard]]
    grab::kernel::TargetObservation
    heuristic_observation()
    {
        return grab::kernel::TargetObservation{
            .grade  = grab::kernel::TargetGrade::Window,
            .alias  = std::nullopt,
            .title  = "Editor",
            .pid    = processId,
            .bounds = bounds,
        };
    }

    [[nodiscard]]
    grab::kernel::AliasEdge
    exact_x11_alias()
    {
        return grab::kernel::AliasEdge{
            .authority  = grab::kernel::AliasAuthority{ std::string{ "x11" } },
            .native_id  = grab::kernel::NativeAliasId{ std::string{ "0x2a" } },
            .confidence = grab::kernel::AliasConfidence::Exact,
            .validity   = grab::kernel::AliasValidity::Active,
        };
    }

    [[nodiscard]]
    grab::SurfaceRecord
    surface_record()
    {
        return grab::SurfaceRecord{
            .id         = surfaceId,
            .generation = grab::DisplayGeneration{ 1U },
            .space      = bounds.space,
            .bounds     = bounds,
        };
    }

}    // namespace

TEST( TargetRegistry,
      IdenticalHeuristicFactsNeverFuseIdentity )
{
    grab::kernel::TargetRegistry registry;

    const auto                   first  = registry.observe( heuristic_observation() );
    const auto                   second = registry.observe( heuristic_observation() );

    ASSERT_TRUE( first.has_value() );
    ASSERT_TRUE( second.has_value() );
    EXPECT_NE( *first, *second );
}

TEST( TargetRegistry,
      ExactActiveAliasFusesIdentityAcrossChangedHeuristics )
{
    grab::kernel::TargetRegistry registry;
    auto                         first_observation = heuristic_observation();
    first_observation.alias                        = exact_x11_alias();

    auto second_observation                        = heuristic_observation();
    second_observation.alias                       = exact_x11_alias();
    second_observation.title                       = "Editor — renamed";
    second_observation.pid                         = std::nullopt;
    second_observation.bounds                      = std::nullopt;

    const auto first  = registry.observe( std::move( first_observation ) );
    const auto second = registry.observe( std::move( second_observation ) );

    ASSERT_TRUE( first.has_value() );
    ASSERT_TRUE( second.has_value() );
    EXPECT_EQ( *first, *second );
}

TEST( TargetRegistry,
      ExactBridgeAttachesAnotherAuthorityToExistingTarget )
{
    grab::kernel::TargetRegistry registry;
    auto                         x11 = heuristic_observation();
    x11.alias                        = exact_x11_alias();
    const auto target                = registry.observe( std::move( x11 ) );
    ASSERT_TRUE( target.has_value() );

    const grab::kernel::AliasEdge atspi_alias{
        .authority  = grab::kernel::AliasAuthority{ std::string{ "at-spi" } },
        .native_id  = grab::kernel::NativeAliasId{ std::string{ "/org/app/1" } },
        .confidence = grab::kernel::AliasConfidence::Exact,
        .validity   = grab::kernel::AliasValidity::Active,
    };
    ASSERT_TRUE( registry.attach_alias( *target, atspi_alias ).has_value() );

    auto semantic_observation  = heuristic_observation();
    semantic_observation.alias = atspi_alias;
    const auto bridged         = registry.observe( std::move( semantic_observation ) );
    ASSERT_TRUE( bridged.has_value() );
    EXPECT_EQ( *bridged, *target );
}

TEST( TargetRegistry,
      InvalidatedAliasDoesNotFuseAReusedNativeId )
{
    grab::kernel::TargetRegistry registry;
    auto                         original = heuristic_observation();
    original.alias                        = exact_x11_alias();
    const auto first                      = registry.observe( std::move( original ) );
    ASSERT_TRUE( first.has_value() );

    const auto alias = exact_x11_alias();
    ASSERT_TRUE(
        registry.invalidate_alias( alias.authority, alias.native_id ).has_value()
    );

    auto replacement  = heuristic_observation();
    replacement.alias = alias;
    const auto second = registry.observe( std::move( replacement ) );
    ASSERT_TRUE( second.has_value() );
    EXPECT_NE( *first, *second );

    const auto retired = registry.target( *first );
    ASSERT_TRUE( retired.has_value() );
    EXPECT_TRUE( std::ranges::any_of( retired->aliases,
                                      []( const grab::kernel::AliasEdge& edge )
                                      {
                                          return edge.validity ==
                                                 grab::kernel::AliasValidity::Inactive;
                                      } ) );
}

TEST( TargetRegistry,
      RegistersAndUpsertsSurfaceForItsTarget )
{
    grab::kernel::TargetRegistry registry;
    const auto                   target = registry.observe( heuristic_observation() );
    ASSERT_TRUE( target.has_value() );

    ASSERT_TRUE( registry.register_surface( *target, surface_record() ).has_value() );

    const grab::SurfaceRecord updated{
        .id         = surfaceId,
        .generation = grab::DisplayGeneration{ 2U },
        .space      = updatedBounds.space,
        .bounds     = updatedBounds,
    };
    ASSERT_TRUE( registry.register_surface( *target, updated ).has_value() );

    const auto record = registry.target( *target );
    ASSERT_TRUE( record.has_value() );
    ASSERT_EQ( record->surfaces.size(), 1U );
    const auto& stored = record->surfaces.front();
    EXPECT_EQ( stored.id, surfaceId );
    EXPECT_EQ( stored.generation, updated.generation );
    EXPECT_EQ( stored.space, updated.space );
    EXPECT_DOUBLE_EQ( stored.bounds.x, updatedBounds.x );
    EXPECT_DOUBLE_EQ( stored.bounds.y, updatedBounds.y );
    EXPECT_DOUBLE_EQ( stored.bounds.w, updatedBounds.w );
    EXPECT_DOUBLE_EQ( stored.bounds.h, updatedBounds.h );
    EXPECT_EQ( stored.bounds.space, updatedBounds.space );
}

TEST( TargetRegistry,
      RejectsZeroSurfaceIdAndCrossTargetOwnership )
{
    grab::kernel::TargetRegistry registry;
    const auto                   first  = registry.observe( heuristic_observation() );
    const auto                   second = registry.observe( heuristic_observation() );
    ASSERT_TRUE( first.has_value() );
    ASSERT_TRUE( second.has_value() );

    auto zero_id       = surface_record();
    zero_id.id         = grab::SurfaceId{};
    const auto invalid = registry.register_surface( *first, zero_id );
    ASSERT_FALSE( invalid.has_value() );
    EXPECT_EQ( invalid.error().code, grab::ErrorCode::InvalidArgument );

    ASSERT_TRUE( registry.register_surface( *first, surface_record() ).has_value() );
    const auto conflicting = registry.register_surface( *second, surface_record() );
    ASSERT_FALSE( conflicting.has_value() );
    EXPECT_EQ( conflicting.error().code, grab::ErrorCode::InvalidArgument );

    const auto second_record = registry.target( *second );
    ASSERT_TRUE( second_record.has_value() );
    EXPECT_TRUE( second_record->surfaces.empty() );
}

TEST( TargetRegistry,
      RemovingSurfaceReleasesItForAnotherTarget )
{
    grab::kernel::TargetRegistry registry;
    const auto                   first  = registry.observe( heuristic_observation() );
    const auto                   second = registry.observe( heuristic_observation() );
    ASSERT_TRUE( first.has_value() );
    ASSERT_TRUE( second.has_value() );

    ASSERT_TRUE( registry.register_surface( *first, surface_record() ).has_value() );
    ASSERT_TRUE( registry.remove_surface( *first, surfaceId ).has_value() );

    const auto first_record = registry.target( *first );
    ASSERT_TRUE( first_record.has_value() );
    EXPECT_TRUE( first_record->surfaces.empty() );

    ASSERT_TRUE( registry.register_surface( *second, surface_record() ).has_value() );
    const auto second_record = registry.target( *second );
    ASSERT_TRUE( second_record.has_value() );
    ASSERT_EQ( second_record->surfaces.size(), 1U );
    EXPECT_EQ( second_record->surfaces.front().id, surfaceId );
}
