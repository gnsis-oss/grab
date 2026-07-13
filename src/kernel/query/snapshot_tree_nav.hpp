#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/ui.hpp"
#include "kernel/query/tree_nav.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace grab::kernel::query
{

    class SnapshotTreeNav final : public TreeNav
    {
        public:

            explicit SnapshotTreeNav( const UiSnapshot& snapshot );

            [[nodiscard]]
            TreeNavMetadata
            metadata() const noexcept override;

            [[nodiscard]]
            std::span<const NodeId>
            nodes() const noexcept override;

            [[nodiscard]]
            std::span<const NodeId>
            roots() const noexcept override;

            [[nodiscard]]
            bool
            contains( NodeId id ) const noexcept override;

            [[nodiscard]]
            RoleId
            role( NodeId id ) const override;

            [[nodiscard]]
            std::uint32_t
            states( NodeId id ) const override;

            [[nodiscard]]
            PropertyRead
            property( NodeId     id,
                      PropertyId property_id ) const override;

            [[nodiscard]]
            std::span<const NodeId>
            children( NodeId id ) const noexcept override;

            [[nodiscard]]
            std::span<const NodeId>
            parents( NodeId id ) const noexcept override;

            [[nodiscard]]
            std::span<const NodeId>
            related( NodeId     id,
                     RelationId relation ) const noexcept override;

            [[nodiscard]]
            std::span<const NodeId>
            related_reverse( NodeId     id,
                             RelationId relation ) const noexcept override;

            [[nodiscard]]
            NodeGeneration
            generation( NodeId id ) const override;

            [[nodiscard]]
            UiProvenance
            provenance( NodeId id ) const override;

        private:

            const UiSnapshot*   snapshot_{};
            std::vector<NodeId> nodes_;
    };

}    // namespace grab::kernel::query
