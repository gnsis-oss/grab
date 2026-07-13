#pragma once

#include "grab/ids.hpp"
#include "grab/result.hpp"

#include <cstdint>
#include <map>

namespace grab::testing
{

    class FakeTreeSource
    {
        public:

            [[nodiscard]]
            WidgetRef
            add_node()
            {
                const WidgetRef ref{
                    .runtime    = runtime_,
                    .tree       = tree_,
                    .epoch      = epoch_,
                    .node       = next_node_++,
                    .generation = NodeGeneration{ 1U },
                };
                generations_.emplace( ref.node, ref.generation );
                return ref;
            }

            void
            bump_epoch() noexcept
            {
                ++epoch_.value;
            }

            void
            bump_generation( const WidgetRef& ref ) noexcept
            {
                const auto generation = generations_.find( ref.node );
                if( generation != generations_.end() )
                {
                    ++generation->second.value;
                }
            }

            [[nodiscard]]
            Result<WidgetRef>
            resolve( const WidgetRef& ref ) const
            {
                if( ref.epoch != epoch_ )
                {
                    return fail( ErrorCode::TreeResynced, "tree epoch changed" );
                }

                const auto generation = generations_.find( ref.node );
                if( generation ==
                    generations_.end() ||
                    generation->second != ref.generation )
                {
                    return fail( ErrorCode::StaleNode, "node generation changed" );
                }

                return ref;
            }

        private:

            RuntimeId                               runtime_{ 1U };
            std::uint32_t                           tree_{ 1U };
            TreeEpoch                               epoch_{ 1U };
            std::uint64_t                           next_node_{ 1U };
            std::map<std::uint64_t, NodeGeneration> generations_;
    };

}
