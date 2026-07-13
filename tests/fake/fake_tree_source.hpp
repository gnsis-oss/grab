#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/ids.hpp"
#include "grab/result.hpp"
#include "spi/tree_source.hpp"

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <utility>
#include <variant>

namespace grab::testing
{

    class FakeTreeSource final : public spi::TreeSource
    {
        public:

            explicit FakeTreeSource( RuntimeId runtime = RuntimeId{ 1U } ) :
                runtime_( runtime )
            {
            }

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
                if( ref.runtime != runtime_ )
                {
                    return fail( ErrorCode::RuntimeRestarted,
                                 "runtime generation changed" );
                }

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

            [[nodiscard]]
            RuntimeId
            runtime_id() const noexcept
            {
                return runtime_;
            }

            void
            restart( RuntimeId runtime ) noexcept
            {
                runtime_              = runtime;
                tree_                 = firstTree;
                epoch_                = TreeEpoch{ firstEpoch };
                next_node_            = firstNode;
                next_source_sequence_ = firstSourceSequence;
                generations_.clear();
                snapshots_.clear();
                script_.clear();
            }

            void
            inject_snapshot( UiSnapshot snapshot )
            {
                snapshots_.insert_or_assign( snapshot.tree, snapshot );
                enqueue( std::move( snapshot ) );
            }

            void
            inject_delta( spi::UiDelta delta )
            {
                enqueue( std::move( delta ) );
            }

            void
            inject_overflow( std::uint64_t dropped )
            {
                const auto lastSequence = next_source_sequence_ == firstSourceSequence
                                            ? noSourceSequence
                                            : next_source_sequence_ - 1U;
                enqueue( spi::TreeGap{
                    .runtime              = runtime_,
                    .tree                 = tree_,
                    .epoch                = epoch_,
                    .last_source_sequence = lastSequence,
                    .dropped              = dropped,
                } );
            }

            void
            inject_partial_commit( UiSnapshot authoritative_after )
            {
                const auto tree = authoritative_after.tree;
                snapshots_.insert_or_assign( tree, std::move( authoritative_after ) );
                script_.emplace_back( Error{
                    .code        = ErrorCode::PossiblyCommitted,
                    .message     = "scripted operation may have committed",
                    .capability  = {},
                    .target      = {},
                    .attempts    = {},
                    .disposition = ErrorDisposition::Fatal,
                    .diagnostics = {},
                } );
            }

            [[nodiscard]]
            Result<UiSnapshot>
            snapshot( std::uint32_t           tree,
                      const OperationContext& context ) override
            {
                const auto contextResult = context.check();
                if( !contextResult.has_value() )
                {
                    return std::unexpected( contextResult.error() );
                }

                const auto found = snapshots_.find( tree );
                if( found == snapshots_.end() )
                {
                    return fail( ErrorCode::NoMatch,
                                 "fake tree snapshot is not available" );
                }
                return found->second;
            }

            [[nodiscard]]
            Result<std::optional<spi::UiUpdate>>
            next_update( const OperationContext& context ) override
            {
                const auto contextResult = context.check();
                if( !contextResult.has_value() )
                {
                    return std::unexpected( contextResult.error() );
                }

                if( script_.empty() )
                {
                    return std::optional<spi::UiUpdate>{};
                }

                auto entry = std::move( script_.front() );
                script_.pop_front();

                if( auto* const error = std::get_if<Error>( &entry ) )
                {
                    return std::unexpected( std::move( *error ) );
                }
                return std::optional<spi::UiUpdate>{
                    std::move( std::get<spi::UiUpdate>( entry ) ),
                };
            }

        private:

            static constexpr std::uint32_t firstTree           = 1U;
            static constexpr std::uint32_t firstEpoch          = 1U;
            static constexpr std::uint64_t firstNode           = 1U;
            static constexpr std::uint64_t firstSourceSequence = 1U;
            static constexpr std::uint64_t noSourceSequence    = 0U;

            template<typename Payload>
            void
            enqueue( Payload payload )
            {
                script_.emplace_back( spi::UiUpdate{
                    .source_sequence = next_source_sequence_++,
                    .payload         = std::move( payload ),
                } );
            }

            using ScriptEntry = std::variant<spi::UiUpdate, Error>;

            RuntimeId     runtime_;
            std::uint32_t tree_{ firstTree };
            TreeEpoch     epoch_{ firstEpoch };
            std::uint64_t next_node_{ firstNode };
            std::uint64_t next_source_sequence_{ firstSourceSequence };
            std::map<std::uint64_t, NodeGeneration> generations_;
            std::map<std::uint32_t, UiSnapshot>     snapshots_;
            std::deque<ScriptEntry>                 script_;
    };

}    // namespace grab::testing
