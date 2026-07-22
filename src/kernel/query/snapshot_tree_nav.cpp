#include "grab/ids.hpp"
#include "grab/relation.hpp"
#include "grab/role.hpp"
#include "grab/ui.hpp"
#include "kernel/query/snapshot_tree_nav.hpp"
#include "kernel/query/tree_nav.hpp"

#include <cstdint>
#include <span>

namespace grab::kernel::query
{

    SnapshotTreeNav::SnapshotTreeNav( const UiSnapshot& snapshot ) :
        snapshot_{ &snapshot }
    {
        nodes_.reserve( snapshot.nodes().size() );
        for( const auto& node : snapshot.nodes() )
        {
            nodes_.push_back( node.id );
        }
    }

    TreeNavMetadata
    SnapshotTreeNav::metadata() const noexcept
    {
        return TreeNavMetadata{
            .runtime  = snapshot_->runtime,
            .tree     = snapshot_->tree,
            .epoch    = snapshot_->epoch,
            .revision = snapshot_->revision,
            .provider = "snapshot",
        };
    }

    std::span<const NodeId>
    SnapshotTreeNav::nodes() const noexcept
    {
        return std::span<const NodeId>{ nodes_ };
    }

    std::span<const NodeId>
    SnapshotTreeNav::roots() const noexcept
    {
        return snapshot_->roots();
    }

    bool
    SnapshotTreeNav::contains( NodeId id ) const noexcept
    {
        return snapshot_->node( id ) != nullptr;
    }

    RoleId
    SnapshotTreeNav::role( NodeId id ) const
    {
        const auto* node = snapshot_->node( id );
        return node == nullptr ? RoleId{} : node->role;
    }

    std::uint32_t
    SnapshotTreeNav::states( NodeId id ) const
    {
        const auto* node = snapshot_->node( id );
        return node == nullptr ? 0U : node->states;
    }

    PropertyRead
    SnapshotTreeNav::property( NodeId     id,
                               PropertyId property_id ) const
    {
        const auto* node = snapshot_->node( id );
        return node == nullptr ? PropertyRead{} : node->property( property_id );
    }

    std::span<const NodeId>
    SnapshotTreeNav::children( NodeId id ) const noexcept
    {
        return snapshot_->related( id, relation::contains );
    }

    std::span<const NodeId>
    SnapshotTreeNav::parents( NodeId id ) const noexcept
    {
        return snapshot_->related_reverse( id, relation::contains );
    }

    std::span<const NodeId>
    SnapshotTreeNav::related( NodeId     id,
                              RelationId relation_id ) const noexcept
    {
        return snapshot_->related( id, relation_id );
    }

    std::span<const NodeId>
    SnapshotTreeNav::related_reverse( NodeId     id,
                                      RelationId relation_id ) const noexcept
    {
        return snapshot_->related_reverse( id, relation_id );
    }

    NodeGeneration
    SnapshotTreeNav::generation( NodeId id ) const
    {
        const auto* node = snapshot_->node( id );
        return node == nullptr ? NodeGeneration{} : node->generation;
    }

    UiProvenance
    SnapshotTreeNav::provenance( NodeId id ) const
    {
        const auto* node = snapshot_->node( id );
        return node == nullptr ? UiProvenance{} : node->provenance();
    }

}    // namespace grab::kernel::query
