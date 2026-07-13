#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "fake/fake_tree_source.hpp"
#include "grab/context.hpp"
#include "grab/ids.hpp"
#include "grab/result.hpp"
#include "grab/ui.hpp"
#include "spi/event_source.hpp"
#include "spi/route.hpp"
#include "spi/runtime.hpp"
#include "spi/tree_source.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace grab::testing
{

    class FakeEventSource final : public spi::EventSource
    {
        public:

            [[nodiscard]]
            Result<void>
            enable( const spi::EventSpec& spec ) override
            {
                ++demand_[spec.name];
                return {};
            }

            [[nodiscard]]
            Result<void>
            disable( const spi::EventSpec& spec ) override
            {
                const auto found = demand_.find( spec.name );
                if( found == demand_.end() )
                {
                    return fail( ErrorCode::InvalidArgument,
                                 "event demand is not enabled" );
                }

                if( found->second == firstDemand )
                {
                    demand_.erase( found );
                }
                else
                {
                    --found->second;
                }
                return {};
            }

            [[nodiscard]]
            std::size_t
            demand_count( const spi::EventSpec& spec ) const noexcept
            {
                const auto found = demand_.find( spec.name );
                return found == demand_.end() ? noDemand : found->second;
            }

            void
            clear() noexcept
            {
                demand_.clear();
            }

        private:

            static constexpr std::size_t       noDemand    = 0U;
            static constexpr std::size_t       firstDemand = 1U;

            std::map<std::string, std::size_t> demand_;
    };

    class FakeRuntime final : public spi::Runtime
    {
        public:

            FakeRuntime() :
                tree_source_( runtime_id_ )
            {
            }

            [[nodiscard]]
            std::string_view
            name() const override
            {
                return "fake";
            }

            [[nodiscard]]
            std::uint32_t
            generation() const override
            {
                return generation_;
            }

            [[nodiscard]]
            RuntimeId
            runtime_id() const noexcept
            {
                return runtime_id_;
            }

            [[nodiscard]]
            Result<void>
            start( const OperationContext& context ) override
            {
                const auto contextResult = context.check();
                if( !contextResult.has_value() )
                {
                    return std::unexpected( contextResult.error() );
                }
                started_ = true;
                return {};
            }

            [[nodiscard]]
            Result<void>
            stop() override
            {
                started_ = false;
                event_source_.clear();
                return {};
            }

            void
            restart() noexcept
            {
                ++generation_;
                ++runtime_id_.value;
                tree_source_.restart( runtime_id_ );
                event_source_.clear();
            }

            [[nodiscard]]
            FakeTreeSource*
            tree_source() override
            {
                return &tree_source_;
            }

            [[nodiscard]]
            FakeEventSource*
            event_source() override
            {
                return &event_source_;
            }

            [[nodiscard]]
            std::span<const spi::RouteDescriptor>
            routes() const override
            {
                return {};
            }

            [[nodiscard]]
            WidgetRef
            add_node()
            {
                return tree_source_.add_node();
            }

            void
            bump_epoch() noexcept
            {
                tree_source_.bump_epoch();
            }

            void
            bump_generation( const WidgetRef& ref ) noexcept
            {
                tree_source_.bump_generation( ref );
            }

            [[nodiscard]]
            Result<WidgetRef>
            resolve( const WidgetRef& ref ) const
            {
                return tree_source_.resolve( ref );
            }

            void
            inject_snapshot( UiSnapshot snapshot )
            {
                tree_source_.inject_snapshot( std::move( snapshot ) );
            }

            void
            inject_delta( spi::UiDelta delta )
            {
                tree_source_.inject_delta( std::move( delta ) );
            }

            void
            inject_overflow( std::uint64_t dropped )
            {
                tree_source_.inject_overflow( dropped );
            }

            void
            inject_partial_commit( UiSnapshot authoritative_after )
            {
                tree_source_.inject_partial_commit( std::move( authoritative_after ) );
            }

        private:

            static constexpr std::uint32_t firstGeneration = 1U;

            std::uint32_t                  generation_{ firstGeneration };
            RuntimeId                      runtime_id_{ firstGeneration };
            bool                           started_{};
            FakeTreeSource                 tree_source_;
            FakeEventSource                event_source_;
    };

}    // namespace grab::testing
