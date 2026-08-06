#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

// A directed graph stored as sorted adjacency lists, keyed by whatever id type
// the caller already uses (NodeId, CoordinateSpaceId, ...) so no side table is
// needed to translate between a graph-internal vertex handle and a grab id.
//
// Both the forward and the reverse adjacency list are kept, so in_edges() is as
// cheap as out_edges(). Each list stays sorted by target, which is what lets
// graph_difference() compare two graphs by a linear merge instead of hashing
// endpoint pairs.

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace grab::kernel
{

    // One outgoing edge: who it points at, and the payload carried on it.
    template<typename Key, typename Payload>
    struct OutgoingEdge
    {
            Key     target{};
            Payload payload{};
    };

    template<typename Key, typename Payload>
    class AdjacencyGraph final
    {
        public:

            using key_type                          = Key;
            using payload_type                      = Payload;
            using edge_type                         = OutgoingEdge<Key, Payload>;

            AdjacencyGraph()                        = default;
            ~AdjacencyGraph()                       = default;
            AdjacencyGraph( const AdjacencyGraph& ) = delete;
            AdjacencyGraph&
            operator=( const AdjacencyGraph& )          = delete;
            AdjacencyGraph( AdjacencyGraph&& ) noexcept = default;
            AdjacencyGraph&
            operator=( AdjacencyGraph&& ) noexcept = default;

            // False when the node is already present; the graph is unchanged.
            [[nodiscard]]
            bool
            add_node( Key node )
            {
                if( outgoing_.contains( node ) )
                {
                    return false;
                }
                outgoing_[node] = {};
                incoming_[node] = {};
                return true;
            }

            // False when either endpoint is absent, when the edge already
            // exists, or when source == target. Self-loops are rejected because
            // every caller here models a containment or transform relation, and
            // a node related to itself is a malformed snapshot rather than a
            // cycle worth representing.
            [[nodiscard]]
            bool
            add_edge( Key     source,
                      Key     target,
                      Payload payload )
            {
                if( source == target )
                {
                    return false;
                }
                const auto from = outgoing_.find( source );
                if( from == outgoing_.end() || !outgoing_.contains( target ) )
                {
                    return false;
                }
                if( find_edge( from->second, target ) != nullptr )
                {
                    return false;
                }

                insert_sorted( from->second, edge_type{ target, payload } );
                insert_sorted( incoming_[target],
                               edge_type{ source, std::move( payload ) } );
                return true;
            }

            // False when the edge is not present.
            [[nodiscard]]
            bool
            remove_edge( Key source,
                         Key target )
            {
                const auto from = outgoing_.find( source );
                if( from ==
                    outgoing_.end() ||
                    find_edge( from->second, target ) == nullptr )
                {
                    return false;
                }
                erase_edge( from->second, target );
                const auto into = incoming_.find( target );
                if( into != incoming_.end() )
                {
                    erase_edge( into->second, source );
                }
                return true;
            }

            // False when the node is not present. Every edge touching it goes
            // with it, in both directions.
            [[nodiscard]]
            bool
            remove_node( Key node )
            {
                const auto from = outgoing_.find( node );
                if( from == outgoing_.end() )
                {
                    return false;
                }

                for( const auto& edge : from->second )
                {
                    const auto into = incoming_.find( edge.target );
                    if( into != incoming_.end() )
                    {
                        erase_edge( into->second, node );
                    }
                }
                const auto into = incoming_.find( node );
                if( into != incoming_.end() )
                {
                    for( const auto& edge : into->second )
                    {
                        const auto source = outgoing_.find( edge.target );
                        if( source != outgoing_.end() )
                        {
                            erase_edge( source->second, node );
                        }
                    }
                    incoming_.erase( into );
                }
                outgoing_.erase( from );
                return true;
            }

            // Edges leaving `node`, ascending by target. Empty when absent.
            [[nodiscard]]
            std::span<const edge_type>
            out_edges( Key node ) const
            {
                return edges_of( outgoing_, node );
            }

            // Edges arriving at `node`, ascending by source. Each entry's
            // `target` is the node the edge comes *from*.
            [[nodiscard]]
            std::span<const edge_type>
            in_edges( Key node ) const
            {
                return edges_of( incoming_, node );
            }

            // Null when the edge is not present.
            [[nodiscard]]
            const Payload*
            edge_payload( Key source,
                          Key target ) const
            {
                const auto from = outgoing_.find( source );
                if( from == outgoing_.end() )
                {
                    return nullptr;
                }
                const auto* edge = find_edge( from->second, target );
                return edge == nullptr ? nullptr : &edge->payload;
            }

            [[nodiscard]]
            bool
            contains_node( Key node ) const
            {
                return outgoing_.contains( node );
            }

            [[nodiscard]]
            bool
            contains_edge( Key source,
                           Key target ) const
            {
                const auto from = outgoing_.find( source );
                return from !=
                       outgoing_.end() &&
                       find_edge( from->second, target ) != nullptr;
            }

            // Every node, ascending. A view over the adjacency map's keys, so
            // enumeration costs nothing beyond the walk itself. The view
            // borrows the graph and must not outlive it.
            [[nodiscard]]
            auto
            nodes() const
            {
                return std::views::keys( outgoing_ );
            }

            [[nodiscard]]
            std::size_t
            node_count() const noexcept
            {
                return outgoing_.size();
            }

            [[nodiscard]]
            std::size_t
            edge_count() const noexcept
            {
                std::size_t total = 0U;
                for( const auto& edges : std::views::values( outgoing_ ) )
                {
                    total += edges.size();
                }
                return total;
            }

            [[nodiscard]]
            bool
            empty() const noexcept
            {
                return outgoing_.empty();
            }

            void
            clear() noexcept
            {
                outgoing_.clear();
                incoming_.clear();
            }

        private:

            using EdgeList = std::vector<edge_type>;

            [[nodiscard]]
            static const edge_type*
            find_edge( const EdgeList& edges,
                       Key             target )
            {
                const auto found = lower_bound( edges, target );
                if( found == edges.end() || found->target != target )
                {
                    return nullptr;
                }
                return &( *found );
            }

            static void
            insert_sorted( EdgeList& edges,
                           edge_type edge )
            {
                const auto at = lower_bound( edges, edge.target );
                edges.insert( at, std::move( edge ) );
            }

            static void
            erase_edge( EdgeList& edges,
                        Key       target )
            {
                const auto found = lower_bound( edges, target );
                if( found != edges.end() && found->target == target )
                {
                    edges.erase( found );
                }
            }

            [[nodiscard]]
            static typename EdgeList::const_iterator
            lower_bound( const EdgeList& edges,
                         Key             target )
            {
                return std::ranges::lower_bound( edges,
                                                 target,
                                                 std::ranges::less{},
                                                 &edge_type::target );
            }

            [[nodiscard]]
            static std::span<const edge_type>
            edges_of( const std::map<Key,
                                     EdgeList>& lists,
                      Key                       node )
            {
                const auto found = lists.find( node );
                if( found == lists.end() )
                {
                    return {};
                }
                return std::span<const edge_type>{ found->second };
            }

            std::map<Key, EdgeList> outgoing_;
            std::map<Key, EdgeList> incoming_;
    };

}    // namespace grab::kernel
