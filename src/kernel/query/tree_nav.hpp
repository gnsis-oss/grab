#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/ui.hpp"

#include <cstdint>
#include <span>
#include <string_view>

namespace grab::kernel::query
{

    struct TreeNavMetadata
    {
            RuntimeId        runtime{};
            std::uint32_t    tree{};
            TreeEpoch        epoch{};
            std::uint64_t    revision{};
            std::string_view provider;
    };

    class TreeNav
    {
        public:

            TreeNav()                 = default;
            virtual ~TreeNav()        = default;
            TreeNav( const TreeNav& ) = delete;
            TreeNav&
            operator=( const TreeNav& ) = delete;
            TreeNav( TreeNav&& )        = delete;
            TreeNav&
            operator=( TreeNav&& ) = delete;

            [[nodiscard]]
            virtual TreeNavMetadata
            metadata() const noexcept = 0;

            [[nodiscard]]
            virtual std::span<const NodeId>
            nodes() const noexcept = 0;

            [[nodiscard]]
            virtual std::span<const NodeId>
            roots() const noexcept = 0;

            [[nodiscard]]
            virtual bool
            contains( NodeId id ) const noexcept = 0;

            [[nodiscard]]
            virtual RoleId
            role( NodeId id ) const = 0;

            [[nodiscard]]
            virtual std::uint32_t
            states( NodeId id ) const = 0;

            [[nodiscard]]
            virtual PropertyRead
            property( NodeId     id,
                      PropertyId property_id ) const = 0;

            [[nodiscard]]
            virtual std::span<const NodeId>
            children( NodeId id ) const noexcept = 0;

            [[nodiscard]]
            virtual std::span<const NodeId>
            parents( NodeId id ) const noexcept = 0;

            [[nodiscard]]
            virtual std::span<const NodeId>
            related( NodeId     id,
                     RelationId relation ) const noexcept = 0;

            [[nodiscard]]
            virtual std::span<const NodeId>
            related_reverse( NodeId     id,
                             RelationId relation ) const noexcept = 0;

            [[nodiscard]]
            virtual NodeGeneration
            generation( NodeId id ) const = 0;

            [[nodiscard]]
            virtual UiProvenance
            provenance( NodeId id ) const = 0;
    };

}    // namespace grab::kernel::query
