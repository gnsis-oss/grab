#pragma once

// ┌──────────────────────────────────────────────────────────────────────┐
// │  walk/strategy.h -- strategy tag structs for algorithm dispatch      │
// └──────────────────────────────────────────────────────────────────────┘
//
// Empty structs used as template parameters for if constexpr dispatch.
// Zero-cost strategy selection, no virtual dispatch.

namespace walk
{

    // ── Path strategies ──────────────────────────────────────────────────
    struct Dijkstra
    {
    };

    struct AStar
    {
    };

    struct BellmanFord
    {
    };

    struct DagPath
    {
    };

    struct FloydWarshall
    {
    };

    // ── Sweep strategies ─────────────────────────────────────────────────
    struct Bfs
    {
    };

    struct Dfs
    {
    };

    // ── Span strategies ──────────────────────────────────────────────────
    struct Prim
    {
    };

    struct Kruskal
    {
    };

    // ── Knot (SCC) strategies ────────────────────────────────────────────
    struct Tarjan
    {
    };

    struct Kosaraju
    {
    };

    // ── Rank strategies ──────────────────────────────────────────────────
    struct Kahn
    {
    };

    struct DfsOrder
    {
    };

    // ── Graft strategies ─────────────────────────────────────────────────
    struct AttachRoots
    {
    };

    struct Splice
    {
    };

    // ── Reduce strategies ────────────────────────────────────────────────
    struct ByRep
    {
    };

    struct ByEquiv
    {
    };

    // ── Empty visitor (default when no visitor is provided) ──────────────
    struct NoVisitor
    {
    };

}    // namespace walk
