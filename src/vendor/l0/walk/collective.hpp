#pragma once
// ┌──────────────────────────────────────────────────────────────────────┐
// │  walk/collective.h -- collective-traversal orderings over peer set   │
// └──────────────────────────────────────────────────────────────────────┘
//
// Pure traversal-order algorithms: ring, binomial / k-ary tree broadcast,
// recursive-doubling partner mapping. No transport, no messaging --
// these produce orderings that consumers (allreduce, allgather, broadcast)
// plumb to whatever transport they use.
//
// Conventions:
//   - rank in [0, n)
//   - n == 0 is treated as a no-op: ring_*/recursive_doubling_partner
//     return rank unchanged; tree-children helpers return empty.

#include <cstddef>
#include <vector>

namespace walk
{

    // ── Ring order ───────────────────────────────────────────────────────

    [[nodiscard]]
    inline std::size_t
    ring_next( std::size_t rank,
               std::size_t n )
    {
        if( n == 0 )
        {
            return rank;
        }
        return ( rank + 1 ) % n;
    }

    [[nodiscard]]
    inline std::size_t
    ring_prev( std::size_t rank,
               std::size_t n )
    {
        if( n == 0 )
        {
            return rank;
        }
        return ( rank + n - 1 ) % n;
    }

    // ── Binomial tree broadcast ──────────────────────────────────────────
    //
    // Binomial broadcast: at step k = 0,1,2,... every rank that already
    // owns the data sends to itself + 2^k. The children of rank r are
    // therefore { r + 2^j : 0 <= j < k_max, r + 2^j < n }, where k_max is
    // the position of the lowest set bit in r (or "infinity" for r == 0).

    [[nodiscard]]
    inline std::vector<std::size_t>
    binomial_tree_children( std::size_t rank,
                            std::size_t n )
    {
        std::vector<std::size_t> children;
        if( rank >= n )
        {
            return children;
        }

        // k_max = position of lowest set bit in rank (or 64 if rank == 0).
        constexpr std::size_t bits_in_size_t = sizeof( std::size_t ) * 8;
        std::size_t           k_max          = bits_in_size_t;
        if( rank != 0 )
        {
            k_max = 0;
            while( ( ( rank >> k_max ) & 1U ) == 0U )
            {
                ++k_max;
            }
        }

        for( std::size_t j = 0; j < k_max; ++j )
        {
            const std::size_t child = rank + ( std::size_t{ 1 } << j );
            if( child >= n )
            {
                break;
            }
            children.push_back( child );
        }
        return children;
    }

    // ── K-ary tree broadcast ─────────────────────────────────────────────
    //
    // Standard array-embedded k-ary tree: children of rank r are
    // k*r + 1, k*r + 2, ..., k*r + k (clipped to < n).

    [[nodiscard]]
    inline std::vector<std::size_t>
    k_ary_tree_children( std::size_t rank,
                         std::size_t n,
                         std::size_t k )
    {
        std::vector<std::size_t> children;
        if( k == 0 || rank >= n )
        {
            return children;
        }
        const std::size_t base = ( k * rank ) + 1;
        for( std::size_t i = 0; i < k; ++i )
        {
            const std::size_t child = base + i;
            if( child >= n )
            {
                break;
            }
            children.push_back( child );
        }
        return children;
    }

    // ── Recursive-doubling partner ───────────────────────────────────────
    //
    // At step s, partner = rank XOR (1 << s). Caller is responsible for
    // handling non-power-of-two peer counts (typical pattern: pre-fold
    // remainder ranks before applying recursive doubling).

    [[nodiscard]]
    inline std::size_t
    recursive_doubling_partner( std::size_t rank,
                                std::size_t step,
                                std::size_t n )
    {
        if( n == 0 )
        {
            return rank;
        }
        return rank ^ ( std::size_t{ 1 } << step );
    }

}    // namespace walk
