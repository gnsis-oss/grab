#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/ids.hpp"
#include "grab/relation.hpp"
#include "grab/role.hpp"
#include "grab/space.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace grab
{

    struct PropertyId
    {
            std::uint32_t value{};
            friend auto
            operator<=>( const PropertyId&,
                         const PropertyId& ) = default;
    };

    namespace property
    {

        inline constexpr PropertyId accessible_name{ 1U };
        inline constexpr PropertyId text{ 2U };
        inline constexpr PropertyId title{ 3U };
        inline constexpr PropertyId window_class{ 4U };
        inline constexpr PropertyId process_id{ 5U };
        inline constexpr PropertyId bounds{ 6U };

    }    // namespace property

    struct NodeId
    {
            std::uint64_t value{};
            friend auto
            operator<=>( const NodeId&,
                         const NodeId& ) = default;
    };

    // Object-scoped semantic behavior advertised by a runtime. These bits
    // describe this node, not a backend-wide capability.
    enum class Facet : std::uint32_t    // NOLINT(performance-enum-size)
    {
        Invokable = 1U << 0U,
        Text      = 1U << 1U,
        Value     = 1U << 2U,
        Selection = 1U << 3U,
    };

    struct FacetMask
    {
            [[nodiscard]]
            constexpr std::uint32_t
            operator()( Facet facet ) const noexcept
            {
                return static_cast<std::uint32_t>( facet );
            }
    };

    inline constexpr FacetMask facet_mask{};

    [[nodiscard]]
    constexpr std::uint32_t
    operator|( Facet left,
               Facet right ) noexcept
    {
        return facet_mask( left ) | facet_mask( right );
    }

    [[nodiscard]]
    constexpr std::uint32_t
    operator|( std::uint32_t facets,
               Facet         facet ) noexcept
    {
        return facets | facet_mask( facet );
    }

    constexpr std::uint32_t&
    operator|=( std::uint32_t& facets,
                Facet          facet ) noexcept
    {
        facets = facets | facet;
        return facets;
    }

    [[nodiscard]]
    constexpr bool
    has_facet( std::uint32_t facets,
               Facet         facet ) noexcept
    {
        return ( facets & facet_mask( facet ) ) != 0U;
    }

    // The uint32_t representation is the stable public bitmask contract.
    enum class NodeState : std::uint32_t    // NOLINT(performance-enum-size)
    {
        Active   = 1U << 0U,
        Focused  = 1U << 1U,
        Visible  = 1U << 2U,
        Selected = 1U << 3U,
        Enabled  = 1U << 4U,
        Editable = 1U << 5U,
        Expanded = 1U << 6U,
        Busy     = 1U << 7U,
    };

    struct StateMask
    {
            [[nodiscard]]
            constexpr std::uint32_t
            operator()( NodeState state ) const noexcept
            {
                return static_cast<std::uint32_t>( state );
            }
    };

    inline constexpr StateMask state_mask{};

    [[nodiscard]]
    constexpr std::uint32_t
    operator|( NodeState left,
               NodeState right ) noexcept
    {
        return state_mask( left ) | state_mask( right );
    }

    [[nodiscard]]
    constexpr std::uint32_t
    operator|( std::uint32_t states,
               NodeState     state ) noexcept
    {
        return states | state_mask( state );
    }

    [[nodiscard]]
    constexpr std::uint32_t
    operator|( NodeState     state,
               std::uint32_t states ) noexcept
    {
        return state_mask( state ) | states;
    }

    constexpr std::uint32_t&
    operator|=( std::uint32_t& states,
                NodeState      state ) noexcept
    {
        states = states | state;
        return states;
    }

    [[nodiscard]]
    constexpr bool
    has_state( std::uint32_t states,
               NodeState     state ) noexcept
    {
        return ( states & state_mask( state ) ) != 0U;
    }

    using PropertyValue =
        std::variant<std::monostate, bool, std::int64_t, double, std::string, SpaceRect>;

    struct PropertyRead
    {
            enum class State : std::uint8_t
            {
                Present,
                Absent,
                Unsupported,
                Uncached,
                BackendFailed,
            };

            State         state{ State::Absent };
            PropertyValue value{};    // NOLINT(readability-redundant-member-init)
    };

    struct UiProperty
    {
            PropertyId   id{};
            PropertyRead read{};
    };

    struct UiProvenance
    {
            RuntimeId     runtime{};
            std::uint64_t revision{};
            friend auto
            operator<=>( const UiProvenance&,
                         const UiProvenance& ) = default;
    };

    class UiNodeRecord
    {
        public:

            UiNodeRecord() = default;

            UiNodeRecord( NodeId                  node_id,
                          NodeGeneration          node_generation,
                          RoleId                  node_role,
                          std::uint32_t           node_states,
                          std::vector<UiProperty> properties,
                          UiProvenance            node_provenance ) :
                UiNodeRecord( node_id,
                              node_generation,
                              node_role,
                              node_states,
                              0U,
                              std::move( properties ),
                              node_provenance )
            {
            }

            UiNodeRecord( NodeId                  node_id,
                          NodeGeneration          node_generation,
                          RoleId                  node_role,
                          std::uint32_t           node_states,
                          std::uint32_t           node_facets,
                          std::vector<UiProperty> properties,
                          UiProvenance            node_provenance ) :
                id( node_id ),
                generation( node_generation ),
                role( node_role ),
                states( node_states ),
                facets( node_facets ),
                provenance_( node_provenance )
            {
                property_ids_.reserve( properties.size() );
                property_reads_.reserve( properties.size() );
                for( auto& property : properties )
                {
                    property_ids_.push_back( property.id );
                    property_reads_.push_back( std::move( property.read ) );
                }
            }

            [[nodiscard]]
            PropertyRead
            property( PropertyId property_id ) const
            {
                for( std::size_t index = 0U; index < property_ids_.size(); ++index )
                {
                    if( property_ids_.at( index ) == property_id )
                    {
                        return property_reads_.at( index );
                    }
                }
                return PropertyRead{};
            }

            [[nodiscard]]
            std::span<const PropertyId>
            property_ids() const noexcept
            {
                return std::span<const PropertyId>{ property_ids_ };
            }

            [[nodiscard]]
            UiProvenance
            provenance() const noexcept
            {
                return provenance_;
            }

            // These are the immutable-record fields named by the public
            // contract; compact property storage remains encapsulated below.
            // NOLINTBEGIN(misc-non-private-member-variables-in-classes,cppcoreguidelines-non-private-member-variables-in-classes)
            NodeId         id{};
            NodeGeneration generation{};
            RoleId         role{};
            std::uint32_t  states{};
            std::uint32_t  facets{};
            // NOLINTEND(misc-non-private-member-variables-in-classes,cppcoreguidelines-non-private-member-variables-in-classes)

        private:

            std::vector<PropertyId>   property_ids_;
            std::vector<PropertyRead> property_reads_;
            UiProvenance              provenance_{};
    };

    struct UiSnapshotMetadata
    {
            RuntimeId     runtime{};
            std::uint32_t tree{};
            TreeEpoch     epoch{};
            std::uint64_t revision{};
            bool          complete{ true };
            friend auto
            operator<=>( const UiSnapshotMetadata&,
                         const UiSnapshotMetadata& ) = default;
    };

    struct UiRelation
    {
            NodeId     source{};
            NodeId     target{};
            RelationId relation{};
            friend auto
            operator<=>( const UiRelation&,
                         const UiRelation& ) = default;
    };

    class UiSnapshot
    {
        public:

            UiSnapshot() = default;

            [[nodiscard]]
            static UiSnapshot
            from_records( UiSnapshotMetadata        metadata,
                          std::vector<UiNodeRecord> nodes,
                          std::vector<NodeId>       root_nodes,
                          std::vector<UiRelation>   relations )
            {
                UiSnapshot snapshot;
                snapshot.runtime    = metadata.runtime;
                snapshot.tree       = metadata.tree;
                snapshot.epoch      = metadata.epoch;
                snapshot.revision   = metadata.revision;
                snapshot.complete   = metadata.complete;
                snapshot.nodes_     = std::move( nodes );
                snapshot.roots_     = std::move( root_nodes );
                snapshot.relations_ = std::move( relations );

                for( const auto& relation_record : snapshot.relations_ )
                {
                    append_related( snapshot.forward_,
                                    relation_record.source,
                                    relation_record.relation,
                                    relation_record.target );
                    append_related( snapshot.reverse_,
                                    relation_record.target,
                                    relation_record.relation,
                                    relation_record.source );
                }
                return snapshot;
            }

            [[nodiscard]]
            const UiNodeRecord*
            node( NodeId node_id ) const noexcept
            {
                for( const auto& node_record : nodes_ )
                {
                    if( node_record.id == node_id )
                    {
                        return &node_record;
                    }
                }
                return nullptr;
            }

            [[nodiscard]]
            std::span<const UiNodeRecord>
            nodes() const noexcept
            {
                return std::span<const UiNodeRecord>{ nodes_ };
            }

            [[nodiscard]]
            std::span<const NodeId>
            roots() const noexcept
            {
                return std::span<const NodeId>{ roots_ };
            }

            [[nodiscard]]
            std::span<const UiRelation>
            relations() const noexcept
            {
                return std::span<const UiRelation>{ relations_ };
            }

            [[nodiscard]]
            std::span<const NodeId>
            related( NodeId     source,
                     RelationId relation_id ) const noexcept
            {
                return find_related( forward_, source, relation_id );
            }

            [[nodiscard]]
            std::span<const NodeId>
            related_reverse( NodeId     target,
                             RelationId relation_id ) const noexcept
            {
                return find_related( reverse_, target, relation_id );
            }

            // Public metadata is intentionally assignable so runtime sources
            // can populate an otherwise empty snapshot incrementally.
            // NOLINTBEGIN(misc-non-private-member-variables-in-classes,cppcoreguidelines-non-private-member-variables-in-classes)
            RuntimeId     runtime{};
            std::uint32_t tree{};
            TreeEpoch     epoch{};
            std::uint64_t revision{};
            bool          complete{ true };
            // NOLINTEND(misc-non-private-member-variables-in-classes,cppcoreguidelines-non-private-member-variables-in-classes)

        private:

            struct RelationBucket
            {
                    NodeId              node{};
                    RelationId          relation{};
                    std::vector<NodeId> related_nodes;
            };

            static void
            append_related( std::vector<RelationBucket>& buckets,
                            NodeId                       node_id,
                            RelationId                   relation_id,
                            NodeId                       related_node )
            {
                for( auto& bucket : buckets )
                {
                    if( bucket.node == node_id && bucket.relation == relation_id )
                    {
                        bucket.related_nodes.push_back( related_node );
                        return;
                    }
                }
                buckets.push_back( RelationBucket{
                    .node          = node_id,
                    .relation      = relation_id,
                    .related_nodes = { related_node },
                } );
            }

            [[nodiscard]]
            static std::span<const NodeId>
            find_related( const std::vector<RelationBucket>& buckets,
                          NodeId                             node_id,
                          RelationId                         relation_id ) noexcept
            {
                for( const auto& bucket : buckets )
                {
                    if( bucket.node == node_id && bucket.relation == relation_id )
                    {
                        return std::span<const NodeId>{ bucket.related_nodes };
                    }
                }
                return {};
            }

            std::vector<UiNodeRecord>   nodes_;
            std::vector<NodeId>         roots_;
            std::vector<UiRelation>     relations_;
            std::vector<RelationBucket> forward_;
            std::vector<RelationBucket> reverse_;
    };

}    // namespace grab
