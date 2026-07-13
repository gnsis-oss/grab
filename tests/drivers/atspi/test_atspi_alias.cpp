#include "drivers/semantic/atspi/atspi_tree_source.hpp"
#include "grab/ids.hpp"
#include "kernel/graph/target_registry.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
// clang-format on

namespace
{

    namespace atspi                      = grab::drivers::semantic::atspi;

    constexpr std::uint32_t processId    = 4'242U;
    constexpr std::uint32_t x11Window    = 42U;
    constexpr const char*   x11Authority = "x11.window.runtime.9";

    [[nodiscard]]
    atspi::AtspiAccessible
    accessible( std::optional<std::uint32_t> bridge )
    {
        return atspi::AtspiAccessible{
            .node         = grab::NodeId{ 71U },
            .generation   = grab::NodeGeneration{ 1U },
            .role         = atspi::AtspiRole::Dialog,
            .interfaces   = atspi::AtspiInterfaceSet{},
            .object_path  = std::string{ "/org/example/preferences" },
            .parent       = std::nullopt,
            .name         = std::string{ "Preferences" },
            .title        = std::string{ "Preferences" },
            .text_content = std::string{},
            .states       = 0U,
            .pid          = processId,
            .bounds       = std::nullopt,
            .x11_window   = bridge,
        };
    }

    [[nodiscard]]
    grab::kernel::TargetObservation
    x11_observation()
    {
        return grab::kernel::TargetObservation{
            .grade = grab::kernel::TargetGrade::Window,
            .alias =
                grab::kernel::AliasEdge{
                                        .authority =
                        grab::kernel::AliasAuthority{
                            std::string{ x11Authority },
                        }, .native_id =
                        grab::kernel::NativeAliasId{
                            std::to_string( x11Window ),
                        }, .confidence = grab::kernel::AliasConfidence::Exact,
                                        .validity   = grab::kernel::AliasValidity::Active,
                                        },
            .title  = std::string{ "Preferences" },
            .pid    = processId,
            .bounds = std::nullopt,
        };
    }

}    // namespace

TEST( AtspiAlias,
      TitleAndPidOnlyRemainASeparateCandidate )
{
    grab::kernel::TargetRegistry registry;
    const auto                   x11_target = registry.observe( x11_observation() );
    ASSERT_TRUE( x11_target.has_value() ) << x11_target.error().message;

    const auto semantic_target =
        atspi::observe_atspi_target( registry,
                                     accessible( std::nullopt ),
                                     std::optional<std::string>{ x11Authority } );
    ASSERT_TRUE( semantic_target.has_value() ) << semantic_target.error().message;
    EXPECT_NE( *semantic_target, *x11_target );
    EXPECT_EQ( registry.size(), 2U );

    const auto semantic_record = registry.target( *semantic_target );
    ASSERT_TRUE( semantic_record.has_value() ) << semantic_record.error().message;
    EXPECT_TRUE( std::ranges::any_of(
        semantic_record->aliases,
        []( const grab::kernel::AliasEdge& edge )
        {
            return edge.confidence == grab::kernel::AliasConfidence::Candidate;
        }
    ) );
}

TEST( AtspiAlias,
      ExactToolkitWindowBridgeFusesWithX11Target )
{
    grab::kernel::TargetRegistry registry;
    const auto                   x11_target = registry.observe( x11_observation() );
    ASSERT_TRUE( x11_target.has_value() ) << x11_target.error().message;

    const auto semantic_target =
        atspi::observe_atspi_target( registry,
                                     accessible( x11Window ),
                                     std::optional<std::string>{ x11Authority } );
    ASSERT_TRUE( semantic_target.has_value() ) << semantic_target.error().message;
    EXPECT_EQ( *semantic_target, *x11_target );
    EXPECT_EQ( registry.size(), 1U );
}
