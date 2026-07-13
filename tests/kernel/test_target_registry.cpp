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
    constexpr grab::SpaceRect bounds{
        .x     = 10.0,
        .y     = 20.0,
        .w     = 800.0,
        .h     = 600.0,
        .space = grab::CoordinateSpaceId{ 3U },
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
