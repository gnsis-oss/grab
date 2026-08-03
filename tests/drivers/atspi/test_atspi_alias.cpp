#include "drivers/desktop/x11/enumerate.hpp"
#include "drivers/desktop/x11/x11_tree_source.hpp"
#include "drivers/semantic/atspi/atspi_tree_source.hpp"
#include "grab/context.hpp"
#include "grab/geometry/rectangle.hpp"
#include "grab/ids.hpp"
#include "grab/result.hpp"
#include "grab/ui.hpp"
#include "kernel/graph/target_registry.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace
{

    namespace atspi = grab::drivers::semantic::atspi;
    namespace x11   = grab::drivers::desktop::x11;

    constexpr grab::RuntimeId         x11Runtime{ 9U };
    constexpr grab::RuntimeId         atspiRuntime{ 10U };
    constexpr grab::DisplayGeneration displayGeneration{ 3U };
    constexpr grab::NodeId            accessibleNode{ 71U };
    constexpr grab::NodeGeneration    accessibleGeneration{ 1U };
    constexpr std::uint32_t           firstTree            = 1U;
    constexpr std::uint32_t           processId            = 4'242U;
    constexpr std::uint32_t           x11Window            = 42U;
    constexpr std::uint32_t           accessibleStates     = 0U;
    constexpr std::size_t             fusedTargetCount     = 1U;
    constexpr std::size_t             separateTargetCount  = 2U;
    constexpr std::string_view        normalWindowType     = "normal";
    constexpr std::string_view        windowTitle          = "Preferences";
    constexpr std::string_view        windowClass          = "GrabPreferences";
    constexpr std::string_view        accessibleObjectPath = "/org/example/preferences";
    constexpr const char*             x11Authority         = "x11.window.runtime.9";
    constexpr const char*             atspiAuthority       = "atspi.object";

    constexpr grab::geometry::Rectangle windowBounds{
        .x      = 10,
        .y      = 20,
        .width  = 800U,
        .height = 600U,
    };

    [[nodiscard]]
    atspi::AtspiAccessible
    accessible( std::optional<std::uint32_t> bridge )
    {
        return atspi::AtspiAccessible{
            .node         = accessibleNode,
            .generation   = accessibleGeneration,
            .role         = atspi::AtspiRole::Dialog,
            .interfaces   = atspi::AtspiInterfaceSet{},
            .object_path  = std::string{ accessibleObjectPath },
            .parent       = std::nullopt,
            .name         = std::string{ windowTitle },
            .title        = std::string{ windowTitle },
            .text_content = std::string{},
            .url          = std::string{},
            .states       = accessibleStates,
            .pid          = processId,
            .bounds       = std::nullopt,
            .x11_window   = bridge,
        };
    }

    [[nodiscard]]
    grab::screen::WindowInfo
    window_info( std::uint32_t                xid,
                 std::string_view             title,
                 std::string_view             window_class,
                 std::optional<std::uint32_t> pid,
                 grab::geometry::Rectangle    bounds )
    {
        return grab::screen::WindowInfo{
            .id       = xid,
            .wm_class = std::string{ window_class },
            .title    = std::string{ title },
            .type     = std::string{ normalWindowType },
            .pid      = pid,
            .bounds   = bounds,
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
            .title  = std::string{ windowTitle },
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
    EXPECT_EQ( registry.size(), separateTargetCount );

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
    EXPECT_EQ( registry.size(), fusedTargetCount );
}

// GoogleTest assertion macros inflate the reported cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST( AtspiAlias,
      SharedRegistryFusesX11TreeSourceAndAtspiBridge )
{
    grab::kernel::TargetRegistry registry;
    x11::X11TreeSource           x11_source{
        x11Runtime,
        displayGeneration,
        registry,
        []() -> grab::Result<std::vector<grab::screen::WindowInfo>>
        {
            return std::vector<grab::screen::WindowInfo>{
                window_info( x11Window,
                             windowTitle,
                             windowClass,
                             processId,
                             windowBounds ),
            };
        },
    };
    const grab::OperationContext context{
        .deadline = grab::Deadline::unbounded(),
    };

    const auto x11_snapshot = x11_source.snapshot( firstTree, context );
    ASSERT_TRUE( x11_snapshot.has_value() ) << x11_snapshot.error().message;
    EXPECT_EQ( registry.size(), fusedTargetCount );

    atspi::AtspiTreeSource atspi_source{
        atspiRuntime,
        registry,
        []() -> grab::Result<std::vector<atspi::AtspiAccessible>>
        {
            return std::vector<atspi::AtspiAccessible>{ accessible( x11Window ) };
        },
        std::optional<std::string>{ x11Authority },
    };
    const auto atspi_snapshot = atspi_source.snapshot( firstTree, context );
    ASSERT_TRUE( atspi_snapshot.has_value() ) << atspi_snapshot.error().message;

    EXPECT_EQ( registry.size(), fusedTargetCount );
    EXPECT_EQ( x11_source.target_registry().size(), fusedTargetCount );

    const auto targets = registry.targets();
    ASSERT_TRUE( targets.has_value() ) << targets.error().message;
    ASSERT_EQ( targets->size(), fusedTargetCount );
    const auto& fused_target = targets->front();
    EXPECT_TRUE( std::ranges::any_of( fused_target.aliases,
                                      []( const grab::kernel::AliasEdge& edge )
                                      {
                                          return edge.authority.value ==
                                                 x11Authority &&
                                                 edge.native_id.value ==
                                                 std::to_string( x11Window ) &&
                                                 edge.confidence ==
                                                 grab::kernel::AliasConfidence::Exact;
                                      } ) );
    EXPECT_TRUE(
        std::ranges::any_of( fused_target.aliases,
                             []( const grab::kernel::AliasEdge& edge )
                             {
                                 return edge.authority.value ==
                                        atspiAuthority &&
                                        edge.confidence ==
                                        grab::kernel::AliasConfidence::Candidate;
                             } )
    );
}
