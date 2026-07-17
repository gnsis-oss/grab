#include "grab/ids.hpp"
#include "grab/relation.hpp"
#include "grab/result.hpp"
#include "grab/role.hpp"
#include "grab/space.hpp"
#include "grab/trace.hpp"
#include "grab/ui.hpp"
#include "kernel/graph/tree_store.hpp"
#include "kernel/support/vendor_adapt.hpp"
#include "spi/tree_source.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <expected>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tag/idx.hpp>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>
#include <walk/diff.hpp>
#include <web/knot.hpp>
#include <web/web.hpp>

namespace grab::kernel
{
    namespace
    {

        constexpr std::size_t maxPropertiesPerNode =
            static_cast<std::size_t>( std::numeric_limits<std::uint8_t>::max() ) + 1U;

        struct NodeTag
        {
        };

        using NodeIndex = tag::Idx<NodeTag, std::uint32_t>;

        struct PropertySlice
        {
                std::size_t   offset{};
                std::uint16_t count{};
        };

        using ExtensionKey = std::tuple<NodeId, NodeId, RelationId>;

        struct Generation
        {
                UiSnapshot                                  snapshot;
                web::Web<web::OneWay, RelationSet>          relations;
                std::map<NodeId, NodeIndex>                 node_indexes;
                std::map<web::Knot, NodeId>                 knot_nodes;
                std::vector<NodeId>                         node_ids;
                std::vector<NodeGeneration>                 node_generations;
                std::vector<RoleId>                         roles;
                std::vector<std::uint32_t>                  states;
                std::vector<PropertySlice>                  property_slices;
                std::vector<PropertyId>                     property_ids;
                std::vector<std::uint8_t>                   property_value_indexes;
                std::vector<PropertyRead>                   property_values;
                std::multimap<ExtensionKey, std::monostate> extension_relations;
        };

        struct PreparedUpdate
        {
                std::unique_ptr<Generation> generation;
                AppliedDelta                applied;
                std::uint64_t               source_sequence{};
        };

        [[nodiscard]]
        UiSnapshotMetadata
        metadata_of( const UiSnapshot& snapshot ) noexcept
        {
            return UiSnapshotMetadata{
                .runtime  = snapshot.runtime,
                .tree     = snapshot.tree,
                .epoch    = snapshot.epoch,
                .revision = snapshot.revision,
                .complete = snapshot.complete,
            };
        }

        [[nodiscard]]
        UiSnapshotMetadata
        metadata_of( const spi::UiUpdate& update,
                     std::uint64_t        current_revision ) noexcept
        {
            if( const auto* snapshot = std::get_if<UiSnapshot>( &update.payload ) )
            {
                return metadata_of( *snapshot );
            }
            if( const auto* delta = std::get_if<spi::UiDelta>( &update.payload ) )
            {
                return UiSnapshotMetadata{
                    .runtime  = delta->runtime,
                    .tree     = delta->tree,
                    .epoch    = delta->epoch,
                    .revision = delta->revision,
                    .complete = delta->complete,
                };
            }

            const auto* gap = std::get_if<spi::TreeGap>( &update.payload );
            if( gap == nullptr )
            {
                return {};
            }
            return UiSnapshotMetadata{
                .runtime  = gap->runtime,
                .tree     = gap->tree,
                .epoch    = gap->epoch,
                .revision = current_revision,
                .complete = false,
            };
        }

        [[nodiscard]]
        std::string
        provenance_text( const UiSnapshotMetadata& metadata )
        {
            return std::string{ "runtime=" } +
                   std::to_string( metadata.runtime.value ) +
                   " tree=" +
                   std::to_string( metadata.tree ) +
                   " epoch=" +
                   std::to_string( metadata.epoch.value ) +
                   " revision=" +
                   std::to_string( metadata.revision );
        }

        [[nodiscard]]
        Error
        rejection( ErrorCode                 code,
                   std::string               message,
                   const UiSnapshotMetadata& metadata )
        {
            const auto provenance = provenance_text( metadata );
            return Error{
                .code       = code,
                .message    = std::move( message ),
                .capability = {},
                .target     = provenance,
                .attempts =
                    {
                               ProviderAttempt{
                            .provider = std::string{ "runtime:" } +
                                        std::to_string( metadata.runtime.value ),
                            .reason   = provenance,
                        }, },
                .disposition = ErrorDisposition::Fatal,
                .diagnostics = {
                               DiagnosticEntry{
                        .at      = {},
                        .message = provenance,
                    }, },
            };
        }

        template<typename T>
        [[nodiscard]]
        Result<T>
        rejected( ErrorCode                 code,
                  std::string               message,
                  const UiSnapshotMetadata& metadata )
        {
            return std::unexpected( rejection( code, std::move( message ), metadata ) );
        }

        [[nodiscard]]
        bool
        property_value_equal( const PropertyValue& left,
                              const PropertyValue& right )
        {
            if( left.index() != right.index() )
            {
                return false;
            }

            if( std::holds_alternative<std::monostate>( left ) )
            {
                return true;
            }
            if( const auto* value = std::get_if<bool>( &left ) )
            {
                return *value == std::get<bool>( right );
            }
            if( const auto* value = std::get_if<std::int64_t>( &left ) )
            {
                return *value == std::get<std::int64_t>( right );
            }
            if( const auto* value = std::get_if<double>( &left ) )
            {
                return *value == std::get<double>( right );
            }
            if( const auto* value = std::get_if<std::string>( &left ) )
            {
                return *value == std::get<std::string>( right );
            }

            const auto& left_rect  = std::get<SpaceRect>( left );
            const auto& right_rect = std::get<SpaceRect>( right );
            return left_rect.x ==
                   right_rect.x &&
                   left_rect.y ==
                   right_rect.y &&
                   left_rect.w ==
                   right_rect.w &&
                   left_rect.h ==
                   right_rect.h &&
                   left_rect.space == right_rect.space;
        }

        [[nodiscard]]
        bool
        property_read_equal( const PropertyRead& left,
                             const PropertyRead& right )
        {
            if( left.state != right.state )
            {
                return false;
            }
            return left.state !=
                   PropertyRead::State::Present ||
                   property_value_equal( left.value, right.value );
        }

        [[nodiscard]]
        std::size_t
        index_value( NodeIndex index ) noexcept
        {
            return static_cast<std::size_t>( index.raw() );
        }

        [[nodiscard]]
        bool
        same_node_facts( const Generation& before,
                         NodeIndex         before_index,
                         const Generation& after,
                         NodeIndex         after_index )
        {
            const auto before_value = index_value( before_index );
            const auto after_value  = index_value( after_index );
            if( before.node_generations.at( before_value ) !=
                after.node_generations.at( after_value ) ||
                before.roles.at( before_value ) !=
                after.roles.at( after_value ) ||
                before.states.at( before_value ) != after.states.at( after_value ) )
            {
                return false;
            }

            const auto before_slice = before.property_slices.at( before_value );
            const auto after_slice  = after.property_slices.at( after_value );
            if( before_slice.count != after_slice.count )
            {
                return false;
            }

            for( std::uint16_t property_index = 0U; property_index < before_slice.count;
                 ++property_index )
            {
                const auto before_offset = before_slice.offset + property_index;
                const auto before_id     = before.property_ids.at( before_offset );
                bool       found         = false;
                for( std::uint16_t candidate_index = 0U;
                     candidate_index < after_slice.count;
                     ++candidate_index )
                {
                    const auto after_offset = after_slice.offset + candidate_index;
                    if( after.property_ids.at( after_offset ) != before_id )
                    {
                        continue;
                    }
                    found = property_read_equal(
                        before.property_values.at(
                            before_slice.offset +
                            before.property_value_indexes.at( before_offset )
                        ),
                        after.property_values.at(
                            after_slice.offset +
                            after.property_value_indexes.at( after_offset )
                        )
                    );
                    break;
                }
                if( !found )
                {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]]
        std::optional<RelationSet>
        edge_relations( const web::Web<web::OneWay,
                                       RelationSet>& graph,
                        NodeId                       source,
                        NodeId                       target )
        {
            for( const auto& neighbor : graph.out( web::Knot{ source.value } ) )
            {
                if( neighbor.target == web::Knot{ target.value } )
                {
                    return neighbor.data;
                }
            }
            return std::nullopt;
        }

        [[nodiscard]]
        // Validation is intentionally one transaction so every rejection gets
        // the same provenance stamp and no partially checked state can escape.
        Result<std::vector<NodeId>>
        // NOLINTNEXTLINE(readability-function-size,readability-function-cognitive-complexity)
        validate_and_find_roots( const UiSnapshotMetadata&     metadata,
                                 std::span<const UiNodeRecord> nodes,
                                 std::span<const NodeId>       supplied_roots,
                                 std::span<const UiRelation>   relations )
        {
            std::map<NodeId, std::size_t> node_positions;
            std::size_t                   node_position{};
            for( const auto& record : nodes )
            {
                if( record.id.value == 0U )
                {
                    return rejected<std::vector<NodeId>>( ErrorCode::InvalidArgument,
                                                          "node id must not be zero",
                                                          metadata );
                }
                if( !node_positions.emplace( record.id, node_position ).second )
                {
                    return rejected<std::vector<NodeId>>(
                        ErrorCode::InvalidArgument,
                        "duplicate node id " + std::to_string( record.id.value ),
                        metadata
                    );
                }

                const auto property_ids = record.property_ids();
                if( property_ids.size() > maxPropertiesPerNode )
                {
                    return rejected<std::vector<NodeId>>(
                        ErrorCode::InvalidArgument,
                        "node exceeds compact property limit",
                        metadata
                    );
                }
                std::set<PropertyId> unique_properties;
                for( const auto property_id : property_ids )
                {
                    if( !unique_properties.insert( property_id ).second )
                    {
                        return rejected<std::vector<NodeId>>(
                            ErrorCode::InvalidArgument,
                            "duplicate property id on node " +
                                std::to_string( record.id.value ),
                            metadata
                        );
                    }
                }
                ++node_position;
            }

            std::set<std::tuple<NodeId, NodeId, RelationId>> unique_relations;
            std::map<NodeId, std::size_t>                    incoming_contains;
            std::map<NodeId, std::vector<NodeId>>            contains_children;
            for( const auto& record : nodes )
            {
                incoming_contains.emplace( record.id, 0U );
                contains_children.emplace( record.id, std::vector<NodeId>{} );
            }

            for( const auto& relation : relations )
            {
                const bool source_known = node_positions.contains( relation.source );
                const bool target_known = node_positions.contains( relation.target );
                if( !source_known || !target_known )
                {
                    const bool unknown_parent =
                        relation.relation == grab::relation::contains && !source_known;
                    return rejected<std::vector<NodeId>>(
                        ErrorCode::NoMatch,
                        unknown_parent ? "unknown parent node"
                                       : "unknown relation endpoint",
                        metadata
                    );
                }
                if( relation.source == relation.target )
                {
                    return rejected<std::vector<NodeId>>(
                        ErrorCode::ProtocolError,
                        relation.relation == grab::relation::contains
                            ? "contains cycle through self"
                            : "self relation is invalid",
                        metadata
                    );
                }
                if( !unique_relations
                         .emplace( relation.source, relation.target, relation.relation )
                         .second )
                {
                    return rejected<std::vector<NodeId>>( ErrorCode::InvalidArgument,
                                                          "duplicate relation",
                                                          metadata );
                }

                const auto relation_value = relation.relation.value;
                if( relation_value >=
                    static_cast<std::uint32_t>( relation::coreCount ) &&
                    relation_value < relation::coreBitCount )
                {
                    return rejected<std::vector<NodeId>>(
                        ErrorCode::InvalidArgument,
                        "relation id is reserved outside the closed core table",
                        metadata
                    );
                }

                if( relation.relation == grab::relation::contains )
                {
                    ++incoming_contains.at( relation.target );
                    contains_children.at( relation.source ).push_back( relation.target );
                }
            }

            std::set<NodeId> supplied_root_set;
            for( const auto root : supplied_roots )
            {
                if( !node_positions.contains( root ) )
                {
                    return rejected<std::vector<NodeId>>( ErrorCode::NoMatch,
                                                          "unknown root node",
                                                          metadata );
                }
                if( !supplied_root_set.insert( root ).second )
                {
                    return rejected<std::vector<NodeId>>( ErrorCode::InvalidArgument,
                                                          "duplicate root node",
                                                          metadata );
                }
                if( incoming_contains.at( root ) != 0U )
                {
                    return rejected<std::vector<NodeId>>( ErrorCode::ProtocolError,
                                                          "root has a contains parent",
                                                          metadata );
                }
            }

            std::deque<NodeId>  ready;
            std::vector<NodeId> roots;
            roots.reserve( nodes.size() );
            for( const auto& [node_id, incoming] : incoming_contains )
            {
                if( incoming == 0U )
                {
                    ready.push_back( node_id );
                    roots.push_back( node_id );
                }
            }

            auto        remaining_incoming = incoming_contains;
            std::size_t visited{};
            while( !ready.empty() )
            {
                const auto current = ready.front();
                ready.pop_front();
                ++visited;
                for( const auto child : contains_children.at( current ) )
                {
                    auto& incoming = remaining_incoming.at( child );
                    --incoming;
                    if( incoming == 0U )
                    {
                        ready.push_back( child );
                    }
                }
            }
            if( visited != nodes.size() )
            {
                return rejected<std::vector<NodeId>>( ErrorCode::ProtocolError,
                                                      "contains relation cycle",
                                                      metadata );
            }
            return roots;
        }

        [[nodiscard]]
        Result<UiSnapshot>
        normalized_snapshot( const UiSnapshotMetadata& metadata,
                             std::vector<UiNodeRecord> nodes,
                             std::span<const NodeId>   supplied_roots,
                             std::vector<UiRelation>   relations )
        {
            const auto roots =
                validate_and_find_roots( metadata, nodes, supplied_roots, relations );
            if( !roots )
            {
                return std::unexpected( roots.error() );
            }
            return UiSnapshot::from_records( metadata,
                                             std::move( nodes ),
                                             *roots,
                                             std::move( relations ) );
        }

        [[nodiscard]]
        Result<std::unique_ptr<Generation>>
        build_generation( UiSnapshot snapshot )
        {
            const auto metadata   = metadata_of( snapshot );
            auto       generation = std::make_unique<Generation>();
            generation->snapshot  = std::move( snapshot );

            const auto nodes      = generation->snapshot.nodes();
            generation->node_ids.reserve( nodes.size() );
            generation->node_generations.reserve( nodes.size() );
            generation->roles.reserve( nodes.size() );
            generation->states.reserve( nodes.size() );
            generation->property_slices.reserve( nodes.size() );

            constexpr auto maxDenseNodes =
                static_cast<std::uint64_t>( std::numeric_limits<std::uint32_t>::max() ) +
                1U;
            if( static_cast<std::uint64_t>( nodes.size() ) > maxDenseNodes )
            {
                return rejected<std::unique_ptr<Generation>>(
                    ErrorCode::InvalidArgument,
                    "tree exceeds dense node index limit",
                    metadata
                );
            }

            std::size_t dense_position{};
            for( const auto& record : nodes )
            {
                const auto dense_index =
                    NodeIndex{ static_cast<std::uint32_t>( dense_position ) };
                generation->node_indexes.emplace( record.id, dense_index );
                generation->knot_nodes.emplace( web::Knot{ record.id.value },
                                                record.id );
                generation->node_ids.push_back( record.id );
                generation->node_generations.push_back( record.generation );
                generation->roles.push_back( record.role );
                generation->states.push_back( record.states );

                const auto ids = record.property_ids();
                generation->property_slices.push_back( PropertySlice{
                    .offset = generation->property_ids.size(),
                    .count  = static_cast<std::uint16_t>( ids.size() ),
                } );
                std::size_t property_index{};
                for( const auto property_id : ids )
                {
                    generation->property_ids.push_back( property_id );
                    generation->property_value_indexes.push_back(
                        static_cast<std::uint8_t>( property_index )
                    );
                    generation->property_values.push_back(
                        record.property( property_id )
                    );
                    ++property_index;
                }

                auto added = detail::from_put(
                    generation->relations.add( web::Knot{ record.id.value } ),
                    ErrorCode::InternalFault
                );
                if( !added )
                {
                    auto error   = std::move( added.error() );
                    error.target = provenance_text( metadata );
                    return std::unexpected( std::move( error ) );
                }
                ++dense_position;
            }

            std::map<std::pair<NodeId, NodeId>, RelationSet> core_relations;
            for( const auto& relation : generation->snapshot.relations() )
            {
                const auto bit = relation_bit( relation.relation );
                if( bit != 0U )
                {
                    core_relations[{ relation.source, relation.target }] |= bit;
                }
                else
                {
                    generation->extension_relations.emplace(
                        ExtensionKey{
                            relation.source,
                            relation.target,
                            relation.relation
                        },
                        std::monostate{}
                    );
                }
            }

            for( const auto& [endpoints, relation_set] : core_relations )
            {
                auto tied = detail::from_put(
                    generation->relations.tie( web::Knot{ endpoints.first.value },
                                               web::Knot{ endpoints.second.value },
                                               relation_set ),
                    ErrorCode::InternalFault
                );
                if( !tied )
                {
                    auto error   = std::move( tied.error() );
                    error.target = provenance_text( metadata );
                    return std::unexpected( std::move( error ) );
                }
            }
            return generation;
        }

        [[nodiscard]]
        TreeEvent
        node_event( TreeEventKind     kind,
                    const Generation& generation,
                    NodeId            node )
        {
            return TreeEvent{
                .kind     = kind,
                .runtime  = generation.snapshot.runtime,
                .tree     = generation.snapshot.tree,
                .epoch    = generation.snapshot.epoch,
                .revision = generation.snapshot.revision,
                .node     = node,
                .related  = NodeId{ 0U },
                .relation = RelationId{ 0U },
            };
        }

        void
        append_relation_bits( std::vector<TreeEvent>& events,
                              TreeEventKind           kind,
                              const Generation&       generation,
                              NodeId                  source,
                              NodeId                  target,
                              RelationSet             bits )
        {
            constexpr std::uint32_t bitCount = 32U;
            for( std::uint32_t index = 0U; index < bitCount; ++index )
            {
                const RelationSet bit = RelationSet{ 1U } << index;
                if( ( bits & bit ) == 0U )
                {
                    continue;
                }
                events.push_back( TreeEvent{
                    .kind     = kind,
                    .runtime  = generation.snapshot.runtime,
                    .tree     = generation.snapshot.tree,
                    .epoch    = generation.snapshot.epoch,
                    .revision = generation.snapshot.revision,
                    .node     = source,
                    .related  = target,
                    .relation = RelationId{ index },
                } );
            }
        }

        void
        append_edge_added( std::vector<TreeEvent>& events,
                           const Generation&       generation,
                           web::Knot               source,
                           web::Knot               target )
        {
            const auto source_id = generation.knot_nodes.at( source );
            const auto target_id = generation.knot_nodes.at( target );
            const auto bits =
                edge_relations( generation.relations, source_id, target_id );
            if( bits )
            {
                append_relation_bits( events,
                                      TreeEventKind::RelationAdded,
                                      generation,
                                      source_id,
                                      target_id,
                                      *bits );
            }
        }

        void
        append_edge_removed( std::vector<TreeEvent>& events,
                             const Generation&       generation,
                             web::Knot               source,
                             web::Knot               target )
        {
            const auto source_id = generation.knot_nodes.at( source );
            const auto target_id = generation.knot_nodes.at( target );
            const auto bits =
                edge_relations( generation.relations, source_id, target_id );
            if( bits )
            {
                append_relation_bits( events,
                                      TreeEventKind::RelationRemoved,
                                      generation,
                                      source_id,
                                      target_id,
                                      *bits );
            }
        }

        void
        append_extension_event( std::vector<TreeEvent>& events,
                                TreeEventKind           kind,
                                const Generation&       generation,
                                const ExtensionKey&     key )
        {
            events.push_back( TreeEvent{
                .kind     = kind,
                .runtime  = generation.snapshot.runtime,
                .tree     = generation.snapshot.tree,
                .epoch    = generation.snapshot.epoch,
                .revision = generation.snapshot.revision,
                .node     = std::get<0>( key ),
                .related  = std::get<1>( key ),
                .relation = std::get<2>( key ),
            } );
        }

        [[nodiscard]]
        std::vector<TreeEvent>
        derive_same_scope_events( const Generation& before,
                                  const Generation& after )
        {
            std::vector<TreeEvent> events;
            const auto difference = walk::diff( before.relations, after.relations );
            events.reserve( difference.added_knots.size() +
                            difference.removed_knots.size() +
                            difference.added_edges.size() +
                            difference.removed_edges.size() +
                            difference.changed_edges.size() );

            for( const auto knot : difference.removed_knots )
            {
                events.push_back( node_event( TreeEventKind::NodeRemoved,
                                              before,
                                              before.knot_nodes.at( knot ) ) );
            }
            for( const auto knot : difference.added_knots )
            {
                events.push_back( node_event( TreeEventKind::NodeAdded,
                                              after,
                                              after.knot_nodes.at( knot ) ) );
            }

            for( const auto& [node_id, after_index] : after.node_indexes )
            {
                const auto before_index = before.node_indexes.find( node_id );
                if( before_index !=
                    before.node_indexes.end() &&
                    !same_node_facts( before,
                                      before_index->second,
                                      after,
                                      after_index ) )
                {
                    events.push_back(
                        node_event( TreeEventKind::NodeChanged, after, node_id )
                    );
                }
            }

            for( const auto& [source, target] : difference.removed_edges )
            {
                append_edge_removed( events, before, source, target );
            }
            for( const auto& [source, target] : difference.added_edges )
            {
                append_edge_added( events, after, source, target );
            }
            for( const auto& [source, target] : difference.changed_edges )
            {
                const auto source_id = after.knot_nodes.at( source );
                const auto target_id = after.knot_nodes.at( target );
                const auto before_bits =
                    edge_relations( before.relations, source_id, target_id )
                        .value_or( 0U );
                const auto after_bits =
                    edge_relations( after.relations, source_id, target_id )
                        .value_or( 0U );
                append_relation_bits( events,
                                      TreeEventKind::RelationRemoved,
                                      before,
                                      source_id,
                                      target_id,
                                      before_bits & ~after_bits );
                append_relation_bits( events,
                                      TreeEventKind::RelationAdded,
                                      after,
                                      source_id,
                                      target_id,
                                      after_bits & ~before_bits );
            }

            for( const auto& [key, unused] : before.extension_relations )
            {
                ( void )unused;
                if( !after.extension_relations.contains( key ) )
                {
                    append_extension_event( events,
                                            TreeEventKind::RelationRemoved,
                                            before,
                                            key );
                }
            }
            for( const auto& [key, unused] : after.extension_relations )
            {
                ( void )unused;
                if( !before.extension_relations.contains( key ) )
                {
                    append_extension_event( events,
                                            TreeEventKind::RelationAdded,
                                            after,
                                            key );
                }
            }
            return events;
        }

        [[nodiscard]]
        std::vector<TreeEvent>
        derive_replacement_events( const Generation* before,
                                   const Generation& after )
        {
            if( before == nullptr )
            {
                web::Web<web::OneWay, RelationSet> empty;
                std::vector<TreeEvent>             events;
                const auto difference = walk::diff( empty, after.relations );
                events.reserve( difference.added_knots.size() +
                                difference.added_edges.size() +
                                after.extension_relations.size() );
                for( const auto knot : difference.added_knots )
                {
                    events.push_back( node_event( TreeEventKind::NodeAdded,
                                                  after,
                                                  after.knot_nodes.at( knot ) ) );
                }
                for( const auto& [source, target] : difference.added_edges )
                {
                    append_edge_added( events, after, source, target );
                }
                for( const auto& [key, unused] : after.extension_relations )
                {
                    ( void )unused;
                    append_extension_event( events,
                                            TreeEventKind::RelationAdded,
                                            after,
                                            key );
                }
                return events;
            }

            const bool same_scope = before->snapshot.runtime ==
                                    after.snapshot.runtime &&
                                    before->snapshot.tree ==
                                    after.snapshot.tree &&
                                    before->snapshot.epoch == after.snapshot.epoch;
            if( same_scope )
            {
                return derive_same_scope_events( *before, after );
            }

            web::Web<web::OneWay, RelationSet> empty;
            std::vector<TreeEvent>             events;
            const auto removed = walk::diff( before->relations, empty );
            const auto added   = walk::diff( empty, after.relations );
            events.reserve( removed.removed_knots.size() +
                            removed.removed_edges.size() +
                            before->extension_relations.size() +
                            added.added_knots.size() +
                            added.added_edges.size() +
                            after.extension_relations.size() );
            for( const auto knot : removed.removed_knots )
            {
                events.push_back( node_event( TreeEventKind::NodeRemoved,
                                              *before,
                                              before->knot_nodes.at( knot ) ) );
            }
            for( const auto& [source, target] : removed.removed_edges )
            {
                append_edge_removed( events, *before, source, target );
            }
            for( const auto& [key, unused] : before->extension_relations )
            {
                ( void )unused;
                append_extension_event( events,
                                        TreeEventKind::RelationRemoved,
                                        *before,
                                        key );
            }
            for( const auto knot : added.added_knots )
            {
                events.push_back( node_event( TreeEventKind::NodeAdded,
                                              after,
                                              after.knot_nodes.at( knot ) ) );
            }
            for( const auto& [source, target] : added.added_edges )
            {
                append_edge_added( events, after, source, target );
            }
            for( const auto& [key, unused] : after.extension_relations )
            {
                ( void )unused;
                append_extension_event( events,
                                        TreeEventKind::RelationAdded,
                                        after,
                                        key );
            }
            return events;
        }

        [[nodiscard]]
        // Delta assembly is kept atomic and linear before the validated rebuild.
        Result<UiSnapshot>
        // NOLINTNEXTLINE(readability-function-size)
        snapshot_from_delta( const Generation&   current,
                             const spi::UiDelta& delta )
        {
            const UiSnapshotMetadata metadata{
                .runtime  = delta.runtime,
                .tree     = delta.tree,
                .epoch    = delta.epoch,
                .revision = delta.revision,
                .complete = delta.complete,
            };
            std::vector<UiNodeRecord> nodes( current.snapshot.nodes().begin(),
                                             current.snapshot.nodes().end() );
            std::vector<UiRelation>   relations( current.snapshot.relations().begin(),
                                                 current.snapshot.relations().end() );

            std::set<NodeId>          added_ids;
            for( const auto& added : delta.added_nodes )
            {
                if( !added_ids.insert( added.id ).second ||
                    std::ranges::any_of( nodes,
                                         [&added]( const UiNodeRecord& existing )
                                         {
                                             return existing.id == added.id;
                                         } ) )
                {
                    return rejected<UiSnapshot>( ErrorCode::InvalidArgument,
                                                 "delta adds duplicate node id",
                                                 metadata );
                }
                nodes.push_back( added );
            }

            std::set<NodeId> changed_ids;
            for( const auto& changed : delta.changed_nodes )
            {
                if( !changed_ids.insert( changed.id ).second )
                {
                    return rejected<UiSnapshot>( ErrorCode::InvalidArgument,
                                                 "delta changes duplicate node id",
                                                 metadata );
                }
                const auto existing =
                    std::ranges::find_if( nodes,
                                          [&changed]( const UiNodeRecord& record )
                                          {
                                              return record.id == changed.id;
                                          } );
                if( existing == nodes.end() )
                {
                    return rejected<UiSnapshot>( ErrorCode::NoMatch,
                                                 "delta changes unknown node",
                                                 metadata );
                }
                *existing = changed;
            }

            std::set<NodeId> removed_ids;
            for( const auto removed : delta.removed_nodes )
            {
                if( !removed_ids.insert( removed ).second )
                {
                    return rejected<UiSnapshot>( ErrorCode::InvalidArgument,
                                                 "delta removes duplicate node id",
                                                 metadata );
                }
                const auto original_size = nodes.size();
                std::erase_if( nodes,
                               [removed]( const UiNodeRecord& record )
                               {
                                   return record.id == removed;
                               } );
                if( nodes.size() == original_size )
                {
                    return rejected<UiSnapshot>( ErrorCode::NoMatch,
                                                 "delta removes unknown node",
                                                 metadata );
                }
                std::erase_if( relations,
                               [removed]( const UiRelation& relation )
                               {
                                   return relation.source ==
                                          removed ||
                                          relation.target == removed;
                               } );
            }

            for( const auto& change : delta.relation_changes )
            {
                const auto matching = [&change]( const UiRelation& relation )
                {
                    return relation.source ==
                           change.source &&
                           relation.target ==
                           change.target &&
                           relation.relation == change.relation;
                };
                const auto existing = std::ranges::find_if( relations, matching );
                if( change.kind == spi::RelationChangeKind::Add )
                {
                    if( existing != relations.end() )
                    {
                        return rejected<UiSnapshot>( ErrorCode::InvalidArgument,
                                                     "delta adds duplicate relation",
                                                     metadata );
                    }
                    relations.push_back( UiRelation{
                        .source   = change.source,
                        .target   = change.target,
                        .relation = change.relation,
                    } );
                }
                else
                {
                    if( existing == relations.end() )
                    {
                        return rejected<UiSnapshot>( ErrorCode::NoMatch,
                                                     "delta removes unknown relation",
                                                     metadata );
                    }
                    relations.erase( existing );
                }
            }

            return normalized_snapshot( metadata,
                                        std::move( nodes ),
                                        {},
                                        std::move( relations ) );
        }

        [[nodiscard]]
        Result<PreparedUpdate>
        prepare_snapshot( const Generation* current,
                          const UiSnapshot& incoming,
                          std::uint64_t     source_sequence )
        {
            const auto metadata = metadata_of( incoming );
            if( current != nullptr )
            {
                const bool same_scope = current->snapshot.runtime ==
                                        incoming.runtime &&
                                        current->snapshot.tree ==
                                        incoming.tree &&
                                        current->snapshot.epoch == incoming.epoch;
                if( same_scope && incoming.revision <= current->snapshot.revision )
                {
                    return rejected<PreparedUpdate>( ErrorCode::ResyncRequired,
                                                     "snapshot revision did not advance",
                                                     metadata );
                }
            }

            std::vector<UiNodeRecord> nodes( incoming.nodes().begin(),
                                             incoming.nodes().end() );
            std::vector<UiRelation>   relations( incoming.relations().begin(),
                                                 incoming.relations().end() );
            const auto normalized = normalized_snapshot( metadata,
                                                         std::move( nodes ),
                                                         incoming.roots(),
                                                         std::move( relations ) );
            if( !normalized )
            {
                return std::unexpected( normalized.error() );
            }
            auto generation = build_generation( *normalized );
            if( !generation )
            {
                return std::unexpected( generation.error() );
            }

            const auto previous_revision =
                current == nullptr ? 0U : current->snapshot.revision;
            auto events = derive_replacement_events( current, **generation );
            return PreparedUpdate{
                .generation = std::move( *generation ),
                .applied =
                    AppliedDelta{
                                 .previous_revision = previous_revision,
                                 .revision          = metadata.revision,
                                 .events            = std::move( events ),
                                 },
                .source_sequence = source_sequence,
            };
        }

        [[nodiscard]]
        Result<PreparedUpdate>
        prepare_delta( const Generation*   current,
                       const spi::UiDelta& delta,
                       std::uint64_t       source_sequence )
        {
            const UiSnapshotMetadata metadata{
                .runtime  = delta.runtime,
                .tree     = delta.tree,
                .epoch    = delta.epoch,
                .revision = delta.revision,
                .complete = delta.complete,
            };
            if( current == nullptr )
            {
                return rejected<PreparedUpdate>(
                    ErrorCode::ResyncRequired,
                    "delta received before an initial snapshot",
                    metadata
                );
            }
            if( delta.runtime != current->snapshot.runtime )
            {
                return rejected<PreparedUpdate>( ErrorCode::RuntimeRestarted,
                                                 "delta runtime identity changed",
                                                 metadata );
            }
            if( delta.tree != current->snapshot.tree )
            {
                return rejected<PreparedUpdate>( ErrorCode::ResyncRequired,
                                                 "delta tree identity changed",
                                                 metadata );
            }
            if( delta.epoch != current->snapshot.epoch )
            {
                return rejected<PreparedUpdate>( ErrorCode::TreeResynced,
                                                 "delta tree epoch changed",
                                                 metadata );
            }
            if( delta.base_revision !=
                current->snapshot.revision ||
                delta.revision <= delta.base_revision )
            {
                return rejected<PreparedUpdate>( ErrorCode::ResyncRequired,
                                                 "delta base revision is out of order",
                                                 metadata );
            }

            const auto candidate_snapshot = snapshot_from_delta( *current, delta );
            if( !candidate_snapshot )
            {
                return std::unexpected( candidate_snapshot.error() );
            }
            auto generation = build_generation( *candidate_snapshot );
            if( !generation )
            {
                return std::unexpected( generation.error() );
            }
            auto events = derive_same_scope_events( *current, **generation );
            return PreparedUpdate{
                .generation = std::move( *generation ),
                .applied =
                    AppliedDelta{
                                 .previous_revision = current->snapshot.revision,
                                 .revision          = delta.revision,
                                 .events            = std::move( events ),
                                 },
                .source_sequence = source_sequence,
            };
        }

        struct QueuedEvents
        {
                std::vector<TreeEvent> events;
        };

    }    // namespace

    struct TreeStore::Impl
    {
            mutable std::mutex          mutex;
            std::unique_ptr<Generation> current;
            std::unique_ptr<Generation> previous;
            EventSink                   sink;
            std::set<RuntimeId>         retired_runtimes;
            std::deque<PreparedUpdate>  pending_updates;
            std::uint64_t               last_source_sequence{};
            bool                        requires_resnapshot{};
            bool                        publishing{};
            std::atomic<std::uint64_t>  publication_failures{
                0U
            };    // NOLINT(readability-redundant-member-init)

            [[nodiscard]]
            const Generation*
            staged_generation() const noexcept
            {
                if( pending_updates.empty() )
                {
                    return current.get();
                }
                return pending_updates.back().generation.get();
            }

            void
            drain_publications() noexcept
            {
                for( ;; )
                {
                    QueuedEvents queued;
                    {
                        const std::scoped_lock lock{ mutex };
                        if( pending_updates.empty() )
                        {
                            publishing = false;
                            return;
                        }

                        auto prepared = std::move( pending_updates.front() );
                        pending_updates.pop_front();
                        queued.events = std::move( prepared.applied.events );
                        previous      = std::move( current );
                        current       = std::move( prepared.generation );
                    }

                    if( !sink )
                    {
                        continue;
                    }
                    for( const auto& event : queued.events )
                    {
                        try
                        {
                            sink( event );
                        }
                        catch( ... )
                        {
                            publication_failures.fetch_add( 1U,
                                                            std::memory_order_relaxed );
                        }
                    }
                }
            }
    };

    TreeStore::TreeStore( EventSink sink ) :
        impl_( std::make_unique<Impl>() )
    {
        impl_->sink = std::move( sink );
    }

    TreeStore::~TreeStore() = default;

    Result<AppliedDelta>
    // NOLINTNEXTLINE(readability-function-size)
    TreeStore::apply( const spi::UiUpdate& update ) noexcept
    {
        try
        {
            AppliedDelta applied;
            bool         should_publish{};
            {
                const std::scoped_lock lock{ impl_->mutex };
                const auto*            staged = impl_->staged_generation();
                const auto             metadata =
                    metadata_of( update,
                                 staged == nullptr ? 0U : staged->snapshot.revision );
                if( impl_->retired_runtimes.contains( metadata.runtime ) )
                {
                    return rejected<AppliedDelta>( ErrorCode::RuntimeRestarted,
                                                   "runtime generation has been retired",
                                                   metadata );
                }

                const bool same_runtime =
                    staged != nullptr && metadata.runtime == staged->snapshot.runtime;
                if( same_runtime &&
                    update.source_sequence !=
                    0U &&
                    update.source_sequence <= impl_->last_source_sequence )
                {
                    return rejected<AppliedDelta>( ErrorCode::ResyncRequired,
                                                   "source sequence did not advance",
                                                   metadata );
                }

                if( const auto* gap = std::get_if<spi::TreeGap>( &update.payload ) )
                {
                    if( staged != nullptr && gap->runtime != staged->snapshot.runtime )
                    {
                        return rejected<AppliedDelta>( ErrorCode::RuntimeRestarted,
                                                       "gap runtime identity changed",
                                                       metadata );
                    }
                    if( staged != nullptr && gap->tree != staged->snapshot.tree )
                    {
                        return rejected<AppliedDelta>( ErrorCode::ResyncRequired,
                                                       "gap tree identity changed",
                                                       metadata );
                    }
                    if( staged != nullptr && gap->epoch != staged->snapshot.epoch )
                    {
                        return rejected<AppliedDelta>( ErrorCode::TreeResynced,
                                                       "gap tree epoch changed",
                                                       metadata );
                    }

                    impl_->requires_resnapshot = true;
                    if( update.source_sequence != 0U )
                    {
                        impl_->last_source_sequence = update.source_sequence;
                    }
                    return rejected<AppliedDelta>( ErrorCode::QueueGap,
                                                   "tree update queue dropped " +
                                                       std::to_string( gap->dropped ) +
                                                       " updates",
                                                   metadata );
                }

                const auto* snapshot = std::get_if<UiSnapshot>( &update.payload );
                if( impl_->requires_resnapshot &&
                    ( snapshot == nullptr || !snapshot->complete ) )
                {
                    return rejected<AppliedDelta>(
                        ErrorCode::ResyncRequired,
                        "full snapshot required after tree update queue gap",
                        metadata
                    );
                }

                Result<PreparedUpdate> prepared =
                    snapshot != nullptr
                        ? prepare_snapshot( staged, *snapshot, update.source_sequence )
                        : prepare_delta( staged,
                                         std::get<spi::UiDelta>( update.payload ),
                                         update.source_sequence );
                if( !prepared )
                {
                    return std::unexpected( prepared.error() );
                }

                applied                    = prepared->applied;
                const bool runtime_changed = staged !=
                                             nullptr &&
                                             staged->snapshot.runtime !=
                                             prepared->generation->snapshot.runtime;
                auto       retired         = impl_->retired_runtimes.end();
                bool       inserted_retired{};
                if( runtime_changed )
                {
                    const auto insertion =
                        impl_->retired_runtimes.insert( staged->snapshot.runtime );
                    retired          = insertion.first;
                    inserted_retired = insertion.second;
                }

                try
                {
                    impl_->pending_updates.push_back( std::move( *prepared ) );
                }
                catch( ... )
                {
                    if( inserted_retired )
                    {
                        impl_->retired_runtimes.erase( retired );
                    }
                    throw;
                }

                if( update.source_sequence != 0U || runtime_changed )
                {
                    impl_->last_source_sequence = update.source_sequence;
                }
                if( snapshot != nullptr && snapshot->complete )
                {
                    impl_->requires_resnapshot = false;
                }
                should_publish    = !impl_->publishing;
                impl_->publishing = true;
            }

            if( should_publish )
            {
                impl_->drain_publications();
            }
            return applied;
        }
        catch( const std::exception& error )
        {
            return fail( ErrorCode::InternalFault,
                         std::string{ "tree update failed: " } + error.what() );
        }
        catch( ... )
        {
            return fail( ErrorCode::InternalFault, "tree update failed" );
        }
    }

    std::optional<UiSnapshot>
    TreeStore::snapshot() const
    {
        const std::scoped_lock lock{ impl_->mutex };
        if( impl_->current == nullptr )
        {
            return std::nullopt;
        }
        return impl_->current->snapshot;
    }

    std::optional<UiSnapshot>
    TreeStore::previous_snapshot() const
    {
        const std::scoped_lock lock{ impl_->mutex };
        if( impl_->previous == nullptr )
        {
            return std::nullopt;
        }
        return impl_->previous->snapshot;
    }

    std::uint64_t
    TreeStore::revision() const
    {
        const std::scoped_lock lock{ impl_->mutex };
        return impl_->current == nullptr ? 0U : impl_->current->snapshot.revision;
    }

    std::optional<RelationSet>
    TreeStore::core_relations( NodeId source,
                               NodeId target ) const
    {
        const std::scoped_lock lock{ impl_->mutex };
        if( impl_->current == nullptr )
        {
            return std::nullopt;
        }
        return edge_relations( impl_->current->relations, source, target );
    }

}    // namespace grab::kernel
