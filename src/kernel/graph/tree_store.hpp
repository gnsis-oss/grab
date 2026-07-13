#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/result.hpp"
#include "grab/ui.hpp"
#include "spi/tree_source.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace grab::kernel
{

    using RelationSet = std::uint32_t;

    [[nodiscard]]
    constexpr RelationSet
    relation_bit( RelationId relation ) noexcept
    {
        constexpr std::uint32_t coreRelationLimit = 32U;
        if( relation.value >= coreRelationLimit )
        {
            return 0U;
        }
        return RelationSet{ 1U } << relation.value;
    }

    enum class TreeEventKind : std::uint8_t
    {
        NodeAdded,
        NodeRemoved,
        NodeChanged,
        RelationAdded,
        RelationRemoved,
    };

    struct TreeEvent
    {
            TreeEventKind kind{ TreeEventKind::NodeChanged };
            RuntimeId     runtime{};
            std::uint32_t tree{};
            TreeEpoch     epoch{};
            std::uint64_t revision{};
            NodeId        node{};
            NodeId        related{};
            RelationId    relation{};

            friend bool
            operator==( const TreeEvent&,
                        const TreeEvent& ) = default;
    };

    struct AppliedDelta
    {
            std::uint64_t          previous_revision{};
            std::uint64_t          revision{};
            std::vector<TreeEvent> events;
    };

    class TreeStore
    {
        public:

            using EventSink = std::function<void( const TreeEvent& )>;

            explicit TreeStore( EventSink sink = {} );
            ~TreeStore();

            TreeStore( const TreeStore& ) = delete;
            TreeStore&
            operator=( const TreeStore& ) = delete;
            TreeStore( TreeStore&& )      = delete;
            TreeStore&
            operator=( TreeStore&& ) = delete;

            [[nodiscard]]
            Result<AppliedDelta>
            apply( const spi::UiUpdate& update ) noexcept;

            [[nodiscard]]
            std::optional<UiSnapshot>
            snapshot() const;

            [[nodiscard]]
            std::optional<UiSnapshot>
            previous_snapshot() const;

            [[nodiscard]]
            std::uint64_t
            revision() const;

            [[nodiscard]]
            std::optional<RelationSet>
            core_relations( NodeId source,
                            NodeId target ) const;

        private:

            struct Impl;
            std::unique_ptr<Impl> impl_;
    };

}    // namespace grab::kernel
