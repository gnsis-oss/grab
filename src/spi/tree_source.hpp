#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/context.hpp"
#include "grab/result.hpp"
#include "grab/ui.hpp"

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace grab::spi
{

    enum class RelationChangeKind : std::uint8_t
    {
        Add,
        Remove,
    };

    struct RelationChange
    {
            RelationChangeKind kind{ RelationChangeKind::Add };
            NodeId             source{};
            NodeId             target{};
            RelationId         relation{};
    };

    struct UiDelta
    {
            RuntimeId     runtime{};
            std::uint32_t tree{};
            TreeEpoch     epoch{};
            std::uint64_t base_revision{};
            std::uint64_t revision{};
            bool          complete{ true };
            std::vector<UiNodeRecord>
                added_nodes{};         // NOLINT(readability-redundant-member-init)
            std::vector<UiNodeRecord>
                changed_nodes{};       // NOLINT(readability-redundant-member-init)
            std::vector<NodeId>
                removed_nodes{};       // NOLINT(readability-redundant-member-init)
            std::vector<RelationChange>
                relation_changes{};    // NOLINT(readability-redundant-member-init)
    };

    struct TreeGap
    {
            RuntimeId     runtime{};
            std::uint32_t tree{};
            TreeEpoch     epoch{};
            std::uint64_t last_source_sequence{};
            std::uint64_t dropped{};
    };

    struct UiUpdate
    {
            std::uint64_t                              source_sequence{};
            std::variant<UiSnapshot, UiDelta, TreeGap> payload;
    };

    class TreeSource
    {
        public:

            TreeSource()                    = default;
            virtual ~TreeSource()           = default;
            TreeSource( const TreeSource& ) = delete;
            TreeSource&
            operator=( const TreeSource& ) = delete;
            TreeSource( TreeSource&& )     = delete;
            TreeSource&
            operator=( TreeSource&& ) = delete;

            [[nodiscard]]
            virtual Result<UiSnapshot>
            snapshot( std::uint32_t           tree,
                      const OperationContext& context ) = 0;

            [[nodiscard]]
            virtual Result<std::optional<UiUpdate>>
            next_update( const OperationContext& context ) = 0;
    };

}    // namespace grab::spi
