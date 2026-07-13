#include "grab/ids.hpp"
#include "grab/relation.hpp"
#include "grab/role.hpp"
#include "grab/ui.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
// clang-format on

namespace
{

    constexpr grab::RuntimeId      runtimeId{ 7U };
    constexpr std::uint32_t        treeId = 3U;
    constexpr grab::TreeEpoch      treeEpoch{ 11U };
    constexpr std::uint64_t        revision = 19U;
    constexpr grab::NodeId         rootId{ 100U };
    constexpr grab::NodeId         childId{ 101U };
    constexpr grab::NodeId         missingId{ 999U };
    constexpr grab::PropertyId     nameProperty{ 1U };
    constexpr grab::PropertyId     enabledProperty{ 2U };
    constexpr grab::PropertyId     missingProperty{ 3U };
    constexpr grab::NodeGeneration firstGeneration{ 1U };
    constexpr std::string_view     childName = "Save";

    [[nodiscard]]
    constexpr std::uint32_t
    state_mask( grab::NodeState state ) noexcept
    {
        return static_cast<std::uint32_t>( state );
    }

    [[nodiscard]]
    grab::UiProvenance
    provenance()
    {
        return grab::UiProvenance{
            .runtime  = runtimeId,
            .revision = revision,
        };
    }

    [[nodiscard]]
    grab::UiNodeRecord
    root_node()
    {
        return grab::UiNodeRecord{
            rootId,
            firstGeneration,
            grab::role::window,
            state_mask( grab::NodeState::Visible ),
            {},
            provenance(),
        };
    }

    [[nodiscard]]
    grab::UiNodeRecord
    child_node()
    {
        const auto states = state_mask( grab::NodeState::Visible ) |
                            state_mask( grab::NodeState::Enabled );
        return grab::UiNodeRecord{
            childId,
            firstGeneration,
            grab::role::region,
            states,
            std::vector<grab::UiProperty>{
                                          grab::UiProperty{
                    .id = nameProperty,
                    .read =
                        grab::PropertyRead{
                            .state = grab::PropertyRead::State::Present,
                            .value = std::string{ childName },
                        },
                }, grab::UiProperty{
                    .id = enabledProperty,
                    .read =
                        grab::PropertyRead{
                            .state = grab::PropertyRead::State::Present,
                            .value = true,
                        },
                }, },
            provenance(),
        };
    }

    [[nodiscard]]
    grab::UiSnapshot
    snapshot()
    {
        return grab::UiSnapshot::from_records(
            grab::UiSnapshotMetadata{
                .runtime  = runtimeId,
                .tree     = treeId,
                .epoch    = treeEpoch,
                .revision = revision,
                .complete = true,
        },
            std::vector<grab::UiNodeRecord>{ root_node(), child_node() },
            std::vector<grab::NodeId>{ rootId },
            std::vector<grab::UiRelation>{
                grab::UiRelation{
                    .source   = rootId,
                    .target   = childId,
                    .relation = grab::relation::contains,
                },
                grab::UiRelation{
                    .source   = rootId,
                    .target   = childId,
                    .relation = grab::relation::active_child,
                },
            }
        );
    }

}    // namespace

TEST( UiRoles,
      CoreNamesAreStable )
{
    const auto cases = std::to_array<std::pair<grab::RoleId, std::string_view>>( {
        {grab::role::application, "application"},
        {     grab::role::window,      "window"},
        {   grab::role::document,    "document"},
        {     grab::role::dialog,      "dialog"},
        {      grab::role::panel,       "panel"},
        {        grab::role::tab,         "tab"},
        {     grab::role::region,      "region"},
        {    grab::role::unknown,     "unknown"},
    } );

    for( const auto& [role, expected] : cases )
    {
        EXPECT_EQ( grab::role_name( role ), expected );
    }
}

TEST( UiRelations,
      CoreNamesAreStable )
{
    const auto cases = std::to_array<std::pair<grab::RelationId, std::string_view>>( {
        {     grab::relation::contains,      "contains"},
        {         grab::relation::owns,          "owns"},
        { grab::relation::presented_on,  "presented_on"},
        {     grab::relation::occupies,      "occupies"},
        {     grab::relation::overlays,      "overlays"},
        { grab::relation::active_child,  "active_child"},
        { grab::relation::focus_within,  "focus_within"},
        {       grab::relation::embeds,        "embeds"},
        {     grab::relation::controls,      "controls"},
        {    grab::relation::label_for,     "label_for"},
        {  grab::relation::labelled_by,   "labelled_by"},
        {grab::relation::controlled_by, "controlled_by"},
        {    grab::relation::popup_for,     "popup_for"},
        {     grab::relation::flows_to,      "flows_to"},
    } );

    for( const auto& [relation, expected] : cases )
    {
        EXPECT_EQ( grab::relation_name( relation ), expected );
    }
}

TEST( UiNodeRecord,
      CompactPropertiesPreserveTypedReadsAndIds )
{
    const auto node = child_node();

    const auto name = node.property( nameProperty );
    ASSERT_EQ( name.state, grab::PropertyRead::State::Present );
    ASSERT_TRUE( std::holds_alternative<std::string>( name.value ) );
    EXPECT_EQ( std::get<std::string>( name.value ), childName );

    const auto enabled = node.property( enabledProperty );
    ASSERT_EQ( enabled.state, grab::PropertyRead::State::Present );
    ASSERT_TRUE( std::holds_alternative<bool>( enabled.value ) );
    EXPECT_TRUE( std::get<bool>( enabled.value ) );

    EXPECT_EQ( node.property( missingProperty ).state,
               grab::PropertyRead::State::Absent );

    const auto ids = node.property_ids();
    ASSERT_EQ( ids.size(), 2U );
    EXPECT_EQ( ids.front(), nameProperty );
    EXPECT_EQ( ids.back(), enabledProperty );
}

TEST( UiNodeState,
      BitmaskCompositionChainsAcrossThreeStates )
{
    const auto states =
        grab::NodeState::Visible | grab::NodeState::Enabled | grab::NodeState::Focused;

    EXPECT_TRUE( grab::has_state( states, grab::NodeState::Visible ) );
    EXPECT_TRUE( grab::has_state( states, grab::NodeState::Enabled ) );
    EXPECT_TRUE( grab::has_state( states, grab::NodeState::Focused ) );
}

TEST( UiSnapshot,
      ExposesMetadataNodesRootsAndBothRelationDirections )
{
    const auto ui = snapshot();

    EXPECT_EQ( ui.runtime, runtimeId );
    EXPECT_EQ( ui.tree, treeId );
    EXPECT_EQ( ui.epoch, treeEpoch );
    EXPECT_EQ( ui.revision, revision );
    EXPECT_TRUE( ui.complete );

    const auto* child = ui.node( childId );
    ASSERT_NE( child, nullptr );
    EXPECT_EQ( child->id, childId );
    EXPECT_EQ( child->generation, firstGeneration );
    EXPECT_EQ( child->role, grab::role::region );
    EXPECT_EQ( child->states,
               state_mask( grab::NodeState::Visible ) |
                   state_mask( grab::NodeState::Enabled ) );
    EXPECT_EQ( ui.node( missingId ), nullptr );

    const auto roots = ui.roots();
    ASSERT_EQ( roots.size(), 1U );
    EXPECT_EQ( roots.front(), rootId );

    const auto children = ui.related( rootId, grab::relation::contains );
    ASSERT_EQ( children.size(), 1U );
    EXPECT_EQ( children.front(), childId );

    const auto active = ui.related( rootId, grab::relation::active_child );
    ASSERT_EQ( active.size(), 1U );
    EXPECT_EQ( active.front(), childId );

    const auto parents = ui.related_reverse( childId, grab::relation::contains );
    ASSERT_EQ( parents.size(), 1U );
    EXPECT_EQ( parents.front(), rootId );

    EXPECT_TRUE( ui.related( childId, grab::relation::contains ).empty() );
    EXPECT_TRUE( ui.related_reverse( rootId, grab::relation::contains ).empty() );
}
