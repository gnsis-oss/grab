#pragma once
// ┌──────────────────────────────────────────────────────────────────────┐
// │  web/concept.hpp -- Reach, Reversible, Graph, DenseGraph concepts   │
// └──────────────────────────────────────────────────────────────────────┘
//
// Reach<G>      -- coalgebraic primitive: out-neighbors only.
// Reversible<G> -- bidirectional: + in-neighbors.
// Graph<G>      -- finite enumerable: + has(), size().
// DenseGraph<G> -- + O(1) knot <-> dense index mapping.
//
// Hierarchy:
//   Reach  ->  Reversible  (adds in())
//   Reach  ->  Graph       (adds has(), size())
//              Graph  ->  DenseGraph  (adds dense_id(), dense_size())
//
// Reach enables lazy/generated state spaces (e.g. playbook::StateSpace)
// that can produce successors but have no finite vertex set.

#include <concepts>
#include <cstddef>
#include <ranges>

namespace web
{

    // ── Reach concept ────────────────────────────────────────────────────
    //
    // Foundation — neighbor exploration (coalgebra / transition system).
    // Given a node, produce its successors. The most primitive notion of
    // a traversable structure.

    template<typename G>
    concept Reach = requires( const G& g, G::knot_type k ) {
        typename G::knot_type;
        typename G::edge_type;
        {
            g.out( k )
        } -> std::ranges::forward_range;
    };

    // ── Reversible concept ───────────────────────────────────────────────
    //
    // Bidirectional traversal — adds in-neighbors to Reach.

    template<typename G>
    concept Reversible = Reach<G> && requires( const G& g, G::knot_type k ) {
        {
            g.in( k )
        } -> std::ranges::forward_range;
    };

    // ── Graph concept ────────────────────────────────────────────────────
    //
    // Finite enumerable graph: existence check and vertex count.
    // Both web::Web and web::fast::Csr satisfy this.

    template<typename G>
    concept Graph = Reach<G> && requires( const G& g, G::knot_type k ) {
        {
            g.has( k )
        } -> std::same_as<bool>;
        {
            g.size()
        } -> std::convertible_to<std::size_t>;
    };

    // ── DenseGraph concept ───────────────────────────────────────────────
    //
    // Extends Graph with O(1) knot-to-dense-index and index-space-size.
    // Enables flat vector bookkeeping in algorithms (visited, distance, parent).

    template<typename G>
    concept DenseGraph = Graph<G> && requires( const G& g, G::knot_type k ) {
        {
            g.dense_id( k )
        } -> std::convertible_to<std::size_t>;
        {
            g.dense_size()
        } -> std::convertible_to<std::size_t>;
    };

}    // namespace web
