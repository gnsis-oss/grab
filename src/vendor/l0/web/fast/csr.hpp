#pragma once
// ┌──────────────────────────────────────────────────────────────────────┐
// │  web/fast/csr.h -- Compressed Sparse Row read-only graph           │
// └──────────────────────────────────────────────────────────────────────┘
//
// Two flat arrays: offsets[V+1] + edges[E].
// Neighbors of node i = edges[offsets[i]..offsets[i+1]].
// Build once from a Web or raw data, then read-only iteration is ~5x
// faster due to contiguous memory. SEED_PREFETCH during iteration.

#include <algorithm>
#include <cstddef>
#include <out/detail/platform.hpp>
#include <span>
#include <unordered_map>
#include <vector>
#include <web/knot.hpp>

namespace web::fast
{

    class Csr
    {
            // Knot -> dense index mapping
            std::vector<Knot>                     knots_;    // index -> Knot
            std::unordered_map<Knot, std::size_t> index_;    // Knot -> dense index

            // CSR arrays
            std::vector<std::size_t>              offsets_;    // size = V+1
            std::vector<std::size_t>              edges_;      // dense neighbor indices

            // Materialized Knot neighbors for Graph concept conformance
            // Built once at construction so out() can return a span<const Knot>.
            std::vector<Knot> knot_edges_;    // E entries: Knot per edge

            // knot_edges_[offsets_[i]..offsets_[i+1]) are Knot neighbors of node i

            void
            build_knot_edges()
            {
                knot_edges_.resize( edges_.size() );
                for( std::size_t i = 0; i < edges_.size(); ++i )
                {
                    knot_edges_[i] = knots_[edges_[i]];
                }
            }

        public:

            // ── Concept conformance ───────────────────────────────────────
            using knot_type = Knot;
            using edge_type = void;

            Csr()           = default;

            // Build from adjacency data: each pair is (knot, list of neighbor knots)
            template<typename AdjRange>
            explicit Csr( const AdjRange& adj )
            {
                // First pass: assign dense indices to all knots
                for( const auto& [knot, neighbors] : adj )
                {
                    if( !index_.contains( knot ) )
                    {
                        index_[knot] = knots_.size();
                        knots_.push_back( knot );
                    }
                    for( auto nb : neighbors )
                    {
                        if( !index_.contains( nb ) )
                        {
                            index_[nb] = knots_.size();
                            knots_.push_back( nb );
                        }
                    }
                }

                std::size_t V = knots_.size();
                offsets_.resize( V + 1, 0 );

                // Count edges per node
                for( const auto& [knot, neighbors] : adj )
                {
                    std::size_t idx   = index_[knot];
                    offsets_[idx + 1] = neighbors.size();
                }

                // Prefix sum
                for( std::size_t i = 1; i <= V; ++i )
                {
                    offsets_[i] += offsets_[i - 1];
                }

                // Fill edges
                edges_.resize( offsets_[V] );
                std::vector<std::size_t> pos( V, 0 );    // write position per node
                for( const auto& [knot, neighbors] : adj )
                {
                    std::size_t idx = index_[knot];
                    for( auto nb : neighbors )
                    {
                        edges_[offsets_[idx] + pos[idx]] = index_[nb];
                        ++pos[idx];
                    }
                }

                build_knot_edges();
            }

            // Build from Web<AnyWay, void> or Web<OneWay, void>
            template<typename WebType>
            static Csr
            from_web( const WebType& web )
            {
                // Collect adjacency
                struct Adj
                {
                        Knot              knot;
                        std::vector<Knot> neighbors;
                };

                std::vector<Adj> adj_list;

                for( auto k : web.knots() )
                {
                    auto out_span = web.out( k );
                    Adj  a{ k, {} };
                    a.neighbors.reserve( out_span.size() );
                    for( auto nb : out_span )
                    {
                        a.neighbors.push_back( nb );
                    }
                    adj_list.push_back( std::move( a ) );
                }

                Csr result;
                // Assign dense indices
                for( const auto& a : adj_list )
                {
                    if( !result.index_.contains( a.knot ) )
                    {
                        result.index_[a.knot] = result.knots_.size();
                        result.knots_.push_back( a.knot );
                    }
                    for( auto nb : a.neighbors )
                    {
                        if( !result.index_.contains( nb ) )
                        {
                            result.index_[nb] = result.knots_.size();
                            result.knots_.push_back( nb );
                        }
                    }
                }

                std::size_t V = result.knots_.size();
                result.offsets_.resize( V + 1, 0 );

                for( const auto& a : adj_list )
                {
                    std::size_t idx          = result.index_[a.knot];
                    result.offsets_[idx + 1] = a.neighbors.size();
                }

                for( std::size_t i = 1; i <= V; ++i )
                {
                    result.offsets_[i] += result.offsets_[i - 1];
                }

                result.edges_.resize( result.offsets_[V] );
                std::vector<std::size_t> pos( V, 0 );
                for( const auto& a : adj_list )
                {
                    std::size_t idx = result.index_[a.knot];
                    for( auto nb : a.neighbors )
                    {
                        result.edges_[result.offsets_[idx] + pos[idx]] =
                            result.index_[nb];
                        ++pos[idx];
                    }
                }

                result.build_knot_edges();
                return result;
            }

            // ── Graph concept: out() returns Knot neighbors ─────────────────────

            [[nodiscard]]
            std::span<const Knot>
            out( Knot k ) const
            {
                auto it = index_.find( k );
                if( it == index_.end() )
                {
                    return {};
                }
                auto begin = offsets_[it->second];
                auto end   = offsets_[it->second + 1];
                return { knot_edges_.data() + begin, end - begin };
            }

            // ── DenseGraph concept ──────────────────────────────────────────────

            [[nodiscard]]
            std::size_t
            dense_id( Knot k ) const
            {
                return index_.find( k )->second;
            }

            [[nodiscard]]
            std::size_t
            dense_size() const
            {
                return knots_.size();
            }

            // ── Query ───────────────────────────────────────────────────────────

            [[nodiscard]]
            std::size_t
            size() const
            {
                return knots_.size();
            }

            [[nodiscard]]
            std::size_t
            edge_count() const
            {
                return offsets_.empty() ? 0 : offsets_.back();
            }

            [[nodiscard]]
            bool
            has( Knot k ) const
            {
                return index_.contains( k );
            }

            [[nodiscard]]
            bool
            has( Knot from,
                 Knot to ) const
            {
                auto fi = index_.find( from );
                auto ti = index_.find( to );
                if( fi == index_.end() || ti == index_.end() )
                {
                    return false;
                }
                std::size_t from_idx = fi->second;
                std::size_t to_idx   = ti->second;
                auto        begin    = offsets_[from_idx];
                auto        end      = offsets_[from_idx + 1];
                for( std::size_t i = begin; i < end; ++i )
                {
                    if( edges_[i] == to_idx )
                    {
                        return true;
                    }
                }
                return false;
            }

            // ── Iteration (hot path) ────────────────────────────────────────────

            // Iterate all neighbors of dense index i. Returns span of dense indices.
            [[nodiscard]]
            std::span<const std::size_t>
            neighbors( std::size_t dense_idx ) const
            {
                auto begin = offsets_[dense_idx];
                auto end   = offsets_[dense_idx + 1];
                return { edges_.data() + begin, end - begin };
            }

            // Iterate all neighbors of a Knot
            [[nodiscard]]
            std::span<const std::size_t>
            neighbors( Knot k ) const
            {
                auto it = index_.find( k );
                if( it == index_.end() )
                {
                    return {};
                }
                return neighbors( it->second );
            }

            // Iterate all nodes by dense index with prefetch
            // NOLINTBEGIN(cppcoreguidelines-missing-std-forward): fn is invoked
            // per-node, not forwarded so it remains usable across iterations
            template<typename Fn>
            void
            for_each_node( Fn&& fn ) const
            {
                std::size_t V = knots_.size();
                for( std::size_t i = 0; i < V; ++i )
                {
                    // Prefetch next node's offset and edge data
                    if( i + 1 < V )
                    {
                        SEED_PREFETCH( &offsets_[i + 2] );
                        SEED_PREFETCH( &edges_[offsets_[i + 1]] );
                    }
                    fn( i, neighbors( i ) );
                }
            }

            // NOLINTEND(cppcoreguidelines-missing-std-forward)

            [[nodiscard]]
            Knot
            knot_at( std::size_t dense_idx ) const
            {
                return knots_[dense_idx];
            }
    };

}    // namespace web::fast
