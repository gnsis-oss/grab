#include "drivers/desktop/x11/x11_tree_source.hpp"
#include "grab/context.hpp"
#include "grab/geometry/rectangle.hpp"
#include "grab/ids.hpp"
#include "grab/presentation.hpp"
#include "grab/result.hpp"
#include "grab/role.hpp"
#include "grab/space.hpp"
#include "grab/ui.hpp"
#include "kernel/graph/target_registry.hpp"
#include "screen/enumerate.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
// clang-format on

namespace
{

    constexpr grab::RuntimeId           runtimeId{ 7U };
    constexpr grab::DisplayGeneration   displayGeneration{ 3U };
    constexpr grab::TreeEpoch           firstEpoch{ 1U };
    constexpr grab::NodeGeneration      firstGeneration{ 1U };
    constexpr grab::CoordinateSpaceId   rootSpace{ 1U };
    constexpr std::uint32_t             firstTree        = 1U;
    constexpr std::uint32_t             invalidTree      = 2U;
    constexpr std::uint32_t             firstXid         = 101U;
    constexpr std::uint32_t             secondXid        = 202U;
    constexpr std::uint32_t             firstPid         = 4'242U;
    constexpr std::uint64_t             firstRevision    = 1U;
    constexpr std::uint64_t             secondRevision   = 2U;
    constexpr std::uint64_t             retiredRevision  = 3U;
    constexpr std::uint64_t             reusedRevision   = 4U;
    constexpr std::string_view          firstTitle       = "Editor";
    constexpr std::string_view          firstClass       = "GrabEditor";
    constexpr std::string_view          secondTitle      = "Terminal";
    constexpr std::string_view          secondClass      = "GrabTerminal";
    constexpr std::string_view          replacementTitle = "Replacement";

    constexpr grab::geometry::Rectangle firstBounds{
        .x      = 10,
        .y      = 20,
        .width  = 800U,
        .height = 600U,
    };
    constexpr grab::geometry::Rectangle secondBounds{
        .x      = -30,
        .y      = 40,
        .width  = 640U,
        .height = 480U,
    };
    constexpr grab::geometry::Rectangle updatedBounds{
        .x      = 50,
        .y      = 60,
        .width  = 1'024U,
        .height = 768U,
    };

    [[nodiscard]]
    grab::OperationContext
    operation_context()
    {
        return grab::OperationContext{
            .deadline = grab::Deadline::unbounded(),
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
            .pid      = pid,
            .bounds   = bounds,
        };
    }

    void
    expect_bounds( const grab::SpaceRect&           actual,
                   const grab::geometry::Rectangle& expected )
    {
        EXPECT_DOUBLE_EQ( actual.x, static_cast<double>( expected.x ) );
        EXPECT_DOUBLE_EQ( actual.y, static_cast<double>( expected.y ) );
        EXPECT_DOUBLE_EQ( actual.w, static_cast<double>( expected.width ) );
        EXPECT_DOUBLE_EQ( actual.h, static_cast<double>( expected.height ) );
        EXPECT_EQ( actual.space, rootSpace );
    }

}    // namespace

// GoogleTest assertion macros inflate the reported cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST( X11TreeSource,
      SnapshotBuildsWindowNodesAndSurfaces )
{
    const std::vector<grab::screen::WindowInfo> windows{
        window_info( firstXid, firstTitle, firstClass, firstPid, firstBounds ),
        window_info( secondXid, secondTitle, secondClass, std::nullopt, secondBounds ),
    };
    grab::kernel::TargetRegistry               registry;
    grab::drivers::desktop::x11::X11TreeSource source{
        runtimeId,
        displayGeneration,
        registry,
        [&windows]() -> grab::Result<std::vector<grab::screen::WindowInfo>>
        {
            return windows;
        },
    };

    const auto snapshot = source.snapshot( firstTree, operation_context() );
    ASSERT_TRUE( snapshot.has_value() ) << snapshot.error().message;
    EXPECT_EQ( snapshot->runtime, runtimeId );
    EXPECT_EQ( snapshot->tree, firstTree );
    EXPECT_EQ( snapshot->epoch, firstEpoch );
    EXPECT_EQ( snapshot->revision, firstRevision );
    EXPECT_TRUE( snapshot->complete );

    const auto nodes = snapshot->nodes();
    const auto roots = snapshot->roots();
    ASSERT_EQ( nodes.size(), 2U );
    ASSERT_EQ( roots.size(), nodes.size() );
    EXPECT_EQ( roots.front(), nodes.front().id );
    EXPECT_EQ( roots.back(), nodes.back().id );
    EXPECT_NE( nodes.front().id, nodes.back().id );

    const auto& first_node = nodes.front();
    EXPECT_EQ( first_node.generation, firstGeneration );
    EXPECT_EQ( first_node.role, grab::role::window );
    EXPECT_TRUE( grab::has_state( first_node.states, grab::NodeState::Visible ) );
    EXPECT_TRUE( grab::has_state( first_node.states, grab::NodeState::Enabled ) );
    const grab::UiProvenance expected_provenance{
        .runtime  = runtimeId,
        .revision = firstRevision,
    };
    EXPECT_EQ( first_node.provenance(), expected_provenance );

    const auto accessible_name = first_node.property( grab::property::accessible_name );
    ASSERT_EQ( accessible_name.state, grab::PropertyRead::State::Present );
    ASSERT_TRUE( std::holds_alternative<std::string>( accessible_name.value ) );
    EXPECT_EQ( std::get<std::string>( accessible_name.value ), firstTitle );

    const auto title = first_node.property( grab::property::title );
    ASSERT_EQ( title.state, grab::PropertyRead::State::Present );
    ASSERT_TRUE( std::holds_alternative<std::string>( title.value ) );
    EXPECT_EQ( std::get<std::string>( title.value ), firstTitle );

    const auto window_class = first_node.property( grab::property::window_class );
    ASSERT_EQ( window_class.state, grab::PropertyRead::State::Present );
    ASSERT_TRUE( std::holds_alternative<std::string>( window_class.value ) );
    EXPECT_EQ( std::get<std::string>( window_class.value ), firstClass );

    const auto pid = first_node.property( grab::property::process_id );
    ASSERT_EQ( pid.state, grab::PropertyRead::State::Present );
    ASSERT_TRUE( std::holds_alternative<std::int64_t>( pid.value ) );
    EXPECT_EQ( std::get<std::int64_t>( pid.value ),
               static_cast<std::int64_t>( firstPid ) );

    const auto bounds = first_node.property( grab::property::bounds );
    ASSERT_EQ( bounds.state, grab::PropertyRead::State::Present );
    ASSERT_TRUE( std::holds_alternative<grab::SpaceRect>( bounds.value ) );
    expect_bounds( std::get<grab::SpaceRect>( bounds.value ), firstBounds );

    EXPECT_EQ( nodes.back().property( grab::property::process_id ).state,
               grab::PropertyRead::State::Absent );

    const auto targets = source.target_registry().targets();
    ASSERT_TRUE( targets.has_value() ) << targets.error().message;
    ASSERT_EQ( targets->size(), 2U );
    const auto& first_target = targets->front();
    EXPECT_EQ( first_target.grade, grab::kernel::TargetGrade::Window );
    ASSERT_EQ( first_target.aliases.size(), 1U );
    EXPECT_EQ( first_target.aliases.front().authority.value,
               std::string{ "x11.window.runtime.7" } );
    EXPECT_EQ( first_target.aliases.front().native_id.value,
               std::to_string( firstXid ) );
    EXPECT_EQ( first_target.aliases.front().confidence,
               grab::kernel::AliasConfidence::Exact );
    EXPECT_EQ( first_target.aliases.front().validity,
               grab::kernel::AliasValidity::Active );
    ASSERT_EQ( first_target.surfaces.size(), 1U );
    const auto& surface = first_target.surfaces.front();
    EXPECT_EQ( surface.generation, displayGeneration );
    EXPECT_EQ( surface.space, rootSpace );
    expect_bounds( surface.bounds, firstBounds );

    const auto& second_target = targets->back();
    ASSERT_EQ( second_target.surfaces.size(), 1U );
    EXPECT_EQ( second_target.surfaces.front().generation, displayGeneration );
    expect_bounds( second_target.surfaces.front().bounds, secondBounds );
}

// GoogleTest assertion macros inflate the reported cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST( X11TreeSource,
      SnapshotKeepsIdentityThenRetiresAndReplacesReusedXid )
{
    std::vector<grab::screen::WindowInfo> windows{
        window_info( firstXid, firstTitle, firstClass, firstPid, firstBounds ),
    };
    grab::kernel::TargetRegistry               registry;
    grab::drivers::desktop::x11::X11TreeSource source{
        runtimeId,
        displayGeneration,
        registry,
        [&windows]() -> grab::Result<std::vector<grab::screen::WindowInfo>>
        {
            return windows;
        },
    };

    const auto first = source.snapshot( firstTree, operation_context() );
    ASSERT_TRUE( first.has_value() ) << first.error().message;
    ASSERT_EQ( first->roots().size(), 1U );
    const grab::NodeId original_node    = first->roots().front();

    const auto         original_targets = source.target_registry().targets();
    ASSERT_TRUE( original_targets.has_value() ) << original_targets.error().message;
    ASSERT_EQ( original_targets->size(), 1U );
    const auto& original_target = original_targets->front();
    ASSERT_EQ( original_target.surfaces.size(), 1U );
    const grab::kernel::TargetId original_target_id = original_target.id;
    const grab::SurfaceId original_surface_id = original_target.surfaces.front().id;

    windows.front().title                     = std::string{ secondTitle };
    windows.front().bounds                    = updatedBounds;
    const auto updated = source.snapshot( firstTree, operation_context() );
    ASSERT_TRUE( updated.has_value() ) << updated.error().message;
    EXPECT_EQ( updated->revision, secondRevision );
    ASSERT_EQ( updated->roots().size(), 1U );
    EXPECT_EQ( updated->roots().front(), original_node );

    const auto updated_targets = source.target_registry().targets();
    ASSERT_TRUE( updated_targets.has_value() ) << updated_targets.error().message;
    ASSERT_EQ( updated_targets->size(), 1U );
    const auto& updated_target = updated_targets->front();
    EXPECT_EQ( updated_target.id, original_target_id );
    ASSERT_EQ( updated_target.surfaces.size(), 1U );
    EXPECT_EQ( updated_target.surfaces.front().id, original_surface_id );
    expect_bounds( updated_target.surfaces.front().bounds, updatedBounds );

    windows.clear();
    const auto retired = source.snapshot( firstTree, operation_context() );
    ASSERT_TRUE( retired.has_value() ) << retired.error().message;
    EXPECT_EQ( retired->revision, retiredRevision );
    EXPECT_TRUE( retired->nodes().empty() );
    EXPECT_TRUE( retired->roots().empty() );

    const auto retired_targets = source.target_registry().targets();
    ASSERT_TRUE( retired_targets.has_value() ) << retired_targets.error().message;
    ASSERT_EQ( retired_targets->size(), 1U );
    const auto& retired_target = retired_targets->front();
    EXPECT_EQ( retired_target.id, original_target_id );
    EXPECT_TRUE( retired_target.surfaces.empty() );
    ASSERT_EQ( retired_target.aliases.size(), 1U );
    EXPECT_EQ( retired_target.aliases.front().validity,
               grab::kernel::AliasValidity::Inactive );

    windows.push_back(
        window_info( firstXid, replacementTitle, firstClass, std::nullopt, secondBounds )
    );
    const auto reused = source.snapshot( firstTree, operation_context() );
    ASSERT_TRUE( reused.has_value() ) << reused.error().message;
    EXPECT_EQ( reused->revision, reusedRevision );
    ASSERT_EQ( reused->roots().size(), 1U );
    EXPECT_NE( reused->roots().front(), original_node );

    const auto reused_targets = source.target_registry().targets();
    ASSERT_TRUE( reused_targets.has_value() ) << reused_targets.error().message;
    ASSERT_EQ( reused_targets->size(), 2U );
    const auto& replacement_target = reused_targets->back();
    EXPECT_NE( replacement_target.id, original_target_id );
    ASSERT_EQ( replacement_target.surfaces.size(), 1U );
    EXPECT_NE( replacement_target.surfaces.front().id, original_surface_id );
    ASSERT_EQ( replacement_target.aliases.size(), 1U );
    EXPECT_EQ( replacement_target.aliases.front().validity,
               grab::kernel::AliasValidity::Active );
}

TEST( X11TreeSource,
      ResolveXidRejectsMismatchedAndStaleWidgetRefs )
{
    std::vector<grab::screen::WindowInfo> windows{
        window_info( firstXid, firstTitle, firstClass, firstPid, firstBounds ),
    };
    grab::kernel::TargetRegistry               registry;
    grab::drivers::desktop::x11::X11TreeSource source{
        runtimeId,
        displayGeneration,
        registry,
        [&windows]() -> grab::Result<std::vector<grab::screen::WindowInfo>>
        {
            return windows;
        },
    };

    const auto snapshot = source.snapshot( firstTree, operation_context() );
    ASSERT_TRUE( snapshot.has_value() ) << snapshot.error().message;
    ASSERT_EQ( snapshot->nodes().size(), 1U );
    const auto&           node = snapshot->nodes().front();
    const grab::WidgetRef widget{
        .runtime    = snapshot->runtime,
        .tree       = snapshot->tree,
        .epoch      = snapshot->epoch,
        .node       = node.id.value,
        .generation = node.generation,
    };

    const auto xid = source.resolve_xid( widget );
    ASSERT_TRUE( xid.has_value() ) << xid.error().message;
    EXPECT_EQ( *xid, firstXid );

    auto mismatched            = widget;
    mismatched.runtime         = grab::RuntimeId{ runtimeId.value + 1U };
    const auto mismatch_result = source.resolve_xid( mismatched );
    ASSERT_FALSE( mismatch_result.has_value() );
    EXPECT_EQ( mismatch_result.error().code, grab::ErrorCode::NoMatch );

    windows.clear();
    const auto retired = source.snapshot( firstTree, operation_context() );
    ASSERT_TRUE( retired.has_value() ) << retired.error().message;
    const auto stale = source.resolve_xid( widget );
    ASSERT_FALSE( stale.has_value() );
    EXPECT_EQ( stale.error().code, grab::ErrorCode::NoMatch );
}

// GoogleTest assertion macros inflate the reported cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST( X11TreeSource,
      InvalidTreeAndDuplicateXidsDoNotMutateState )
{
    std::vector<grab::screen::WindowInfo> windows{
        window_info( firstXid, firstTitle, firstClass, firstPid, firstBounds ),
        window_info( firstXid, secondTitle, secondClass, std::nullopt, secondBounds ),
    };
    grab::kernel::TargetRegistry               registry;
    std::size_t                                enumerate_count = 0U;
    grab::drivers::desktop::x11::X11TreeSource source{
        runtimeId,
        displayGeneration,
        registry,
        [&windows,
         &enumerate_count]() -> grab::Result<std::vector<grab::screen::WindowInfo>>
        {
            ++enumerate_count;
            return windows;
        },
    };

    const auto invalid = source.snapshot( invalidTree, operation_context() );
    ASSERT_FALSE( invalid.has_value() );
    EXPECT_EQ( invalid.error().code, grab::ErrorCode::NoMatch );
    EXPECT_EQ( enumerate_count, 0U );
    EXPECT_EQ( source.target_registry().size(), 0U );

    const auto duplicate = source.snapshot( firstTree, operation_context() );
    ASSERT_FALSE( duplicate.has_value() );
    EXPECT_EQ( duplicate.error().code, grab::ErrorCode::ProtocolError );
    EXPECT_EQ( enumerate_count, 1U );
    EXPECT_EQ( source.target_registry().size(), 0U );

    windows.pop_back();
    const auto valid = source.snapshot( firstTree, operation_context() );
    ASSERT_TRUE( valid.has_value() ) << valid.error().message;
    EXPECT_EQ( valid->revision, firstRevision );
    EXPECT_EQ( enumerate_count, 2U );
    EXPECT_EQ( source.target_registry().size(), 1U );
}
