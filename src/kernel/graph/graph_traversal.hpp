#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

// Breadth- and depth-first traversal over anything that can name its
// out-neighbours. Both take the visitor by mutable reference so it can
// accumulate; both visit each node exactly once and do nothing at all when the
// start node is absent.
//
// A graph only has to satisfy TraversableGraph. When it can also map a node to
// a dense index (IndexedGraph) the visited set becomes a flat vector<bool>
// rather than a tree, which is the difference between a pointer chase and an
// array read on every edge examined.

#include <concepts>
#include <cstddef>
#include <ranges>
#include <set>
#include <type_traits>
#include <utility>
#include <vector>

namespace grab::kernel
{

    // out_edges() may yield bare keys (an adapter over an existing tree) or
    // OutgoingEdge-shaped entries (AdjacencyGraph). Both are traversable.
    template<typename Key,
             typename Entry>
    [[nodiscard]]
    constexpr Key
    edge_target( const Entry& entry )
    {
        if constexpr( std::same_as<std::remove_cvref_t<Entry>, Key> )
        {
            return entry;
        }
        else
        {
            return entry.target;
        }
    }

    template<typename G>
    concept TraversableGraph = requires( const G& graph, typename G::key_type node ) {
        typename G::key_type;
        {
            graph.out_edges( node )
        } -> std::ranges::forward_range;
        {
            graph.contains_node( node )
        } -> std::same_as<bool>;
    };

    // A graph that can number its nodes 0..index_space_size(), so traversal can
    // track "seen" in a flat array.
    template<typename G>
    concept IndexedGraph =
        TraversableGraph<G> && requires( const G& graph, typename G::key_type node ) {
            {
                graph.node_index( node )
            } -> std::convertible_to<std::size_t>;
            {
                graph.index_space_size()
            } -> std::convertible_to<std::size_t>;
        };

    // visit_node() is called once per node, in traversal order. visit_edge() is
    // optional: when the visitor defines it, it is called for every edge
    // examined, including edges into already-visited nodes.
    template<typename V, typename Key>
    concept GraphVisitor = requires( V& visitor, Key node ) {
        {
            visitor.visit_node( node )
        } -> std::same_as<void>;
    };

    namespace detail
    {

        // Chooses flat-array or tree-backed bookkeeping and hands `body` a pair
        // of lambdas, so neither traversal has to spell the choice out twice.
        template<typename G,
                 typename Body>
        void
        with_visited_set( const G& graph,
                          Body&&   body )
        {
            using Key = typename G::key_type;

            if constexpr( IndexedGraph<G> )
            {
                std::vector<bool> seen( graph.index_space_size(), false );
                body(
                    [&]( Key node )
                    {
                        return seen[graph.node_index( node )];
                    },
                    [&]( Key node )
                    {
                        seen[graph.node_index( node )] = true;
                    }
                );
            }
            else
            {
                std::set<Key> seen;
                body(
                    [&]( Key node )
                    {
                        return seen.contains( node );
                    },
                    [&]( Key node )
                    {
                        seen.insert( node );
                    }
                );
            }
        }

        template<typename V,
                 typename Key>
        void
        offer_edge( V&  visitor,
                    Key source,
                    Key target )
        {
            if constexpr( requires { visitor.visit_edge( source, target ); } )
            {
                visitor.visit_edge( source, target );
            }
            else
            {
                ( void )visitor;
                ( void )source;
                ( void )target;
            }
        }

    }    // namespace detail

    template<TraversableGraph G,
             typename V>
    requires GraphVisitor<V,
                          typename G::key_type>
    void
    breadth_first_search( const G&             graph,
                          typename G::key_type start,
                          V&                   visitor )
    {
        using Key = typename G::key_type;

        if( !graph.contains_node( start ) )
        {
            return;
        }

        detail::with_visited_set(
            graph,
            [&]( auto seen, auto mark )
            {
                std::vector<Key> frontier{ start };
                std::vector<Key> next;
                mark( start );

                while( !frontier.empty() )
                {
                    for( const Key node : frontier )
                    {
                        visitor.visit_node( node );
                        for( const auto& entry : graph.out_edges( node ) )
                        {
                            const Key neighbour = edge_target<Key>( entry );
                            detail::offer_edge( visitor, node, neighbour );
                            if( !seen( neighbour ) )
                            {
                                mark( neighbour );
                                next.push_back( neighbour );
                            }
                        }
                    }
                    frontier.swap( next );
                    next.clear();
                }
            }
        );
    }

    template<TraversableGraph G,
             typename V>
    requires GraphVisitor<V,
                          typename G::key_type>
    void
    depth_first_search( const G&             graph,
                        typename G::key_type start,
                        V&                   visitor )
    {
        using Key = typename G::key_type;

        if( !graph.contains_node( start ) )
        {
            return;
        }

        // An explicit stack, not recursion: these graphs come from a foreign
        // process and their depth is not ours to bound.
        detail::with_visited_set(
            graph,
            [&]( auto seen, auto mark )
            {
                std::vector<Key> pending{ start };

                while( !pending.empty() )
                {
                    const Key node = pending.back();
                    pending.pop_back();
                    if( seen( node ) )
                    {
                        continue;
                    }
                    mark( node );
                    visitor.visit_node( node );

                    // Pushed in reverse so the first out-edge is popped first.
                    const auto edges = graph.out_edges( node );
                    for( const auto& entry : edges | std::views::reverse )
                    {
                        const Key neighbour = edge_target<Key>( entry );
                        detail::offer_edge( visitor, node, neighbour );
                        if( !seen( neighbour ) )
                        {
                            pending.push_back( neighbour );
                        }
                    }
                }
            }
        );
    }

}    // namespace grab::kernel
