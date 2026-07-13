#pragma once
// ┌──────────────────────────────────────────────────────────────────────┐
// │  walk/multilevel.h -- Scotch-style multilevel partitioning skeleton  │
// └──────────────────────────────────────────────────────────────────────┘
//
// Recursion template for multilevel domain decomposition. The skeleton
// only orchestrates -- the consumer supplies three strategies:
//
//   matcher(g)
//       -> std::optional<Quotient<Policy, Edge>>
//          Produce a coarsened version of g (e.g. heavy-edge matching).
//          Return std::nullopt to stop coarsening (base case).
//
//   partitioner(g)
//       -> Partition
//          Produce an initial partition of the (coarsest) graph.
//
//   refiner(finer, current_partition, coarse, projection)
//       -> Partition
//          Project a partition from `coarse` (whose vertices appear as
//          values in `projection`) onto `finer`, optionally refining
//          locally (Kernighan-Lin, Fiduccia-Mattheyses, etc.).
//
// The skeleton is deliberately small (~30 lines): all the algorithmic
// content lives in the strategies the consumer supplies.

#include <cstddef>
#include <unordered_map>
#include <utility>
#include <vector>
#include <walk/parts.hpp>
#include <walk/reduce.hpp>
#include <web/knot.hpp>
#include <web/web.hpp>

namespace walk
{

    template<typename Policy,
             typename Edge,
             typename Matcher,
             typename Partitioner,
             typename Refiner>
    [[nodiscard]]
    Partition
    multilevel( const web::Web<Policy,
                               Edge>& g,
                Matcher&              matcher,
                Partitioner&          partitioner,
                Refiner&              refiner )
    {
        // Coarsening phase: keep matching until matcher signals stop.
        // Each level remembers (graph, projection-from-finer-level).
        std::vector<web::Web<Policy, Edge>>                   levels;
        std::vector<std::unordered_map<web::Knot, web::Knot>> projections;

        const web::Web<Policy, Edge>*                         current = &g;
        while( true )
        {
            auto coarsened = matcher( *current );
            if( !coarsened )
            {
                break;
            }
            levels.push_back( std::move( coarsened->graph ) );
            projections.push_back( std::move( coarsened->representative_of ) );
            current = &levels.back();
        }

        // Initial partition on the coarsest graph.
        Partition partition = partitioner( *current );

        // Uncoarsening phase: lift partition back through each projection.
        // Walk levels back to original g; the projection at index i takes
        // knots in level i-1 (or original g for i=0) to knots in level i.
        for( std::size_t step = levels.size(); step-- > 0; )
        {
            const auto& coarse     = levels[step];
            const auto& projection = projections[step];
            const auto& finer      = ( step == 0 ) ? g : levels[step - 1];
            partition = refiner( finer, std::move( partition ), coarse, projection );
        }

        return partition;
    }

}    // namespace walk
