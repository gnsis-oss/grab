#include "kernel/capture/damage_hint_auditor.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace grab::kernel
{
    namespace
    {

        [[nodiscard]]
        bool
        intersects( const geometry::Rectangle& lhs,
                    const geometry::Rectangle& rhs )
        {
            if( lhs.width ==
                0U ||
                lhs.height ==
                0U ||
                rhs.width ==
                0U ||
                rhs.height == 0U )
            {
                return false;
            }
            const auto lhs_left   = static_cast<std::int64_t>( lhs.x );
            const auto lhs_top    = static_cast<std::int64_t>( lhs.y );
            const auto lhs_right  = lhs_left + lhs.width;
            const auto lhs_bottom = lhs_top + lhs.height;
            const auto rhs_left   = static_cast<std::int64_t>( rhs.x );
            const auto rhs_top    = static_cast<std::int64_t>( rhs.y );
            const auto rhs_right  = rhs_left + rhs.width;
            const auto rhs_bottom = rhs_top + rhs.height;
            return lhs_left <
                   rhs_right &&
                   rhs_left <
                   lhs_right &&
                   lhs_top <
                   rhs_bottom &&
                   rhs_top < lhs_bottom;
        }

        [[nodiscard]]
        double
        calculate_miss_rate( std::span<const geometry::Rectangle> hints,
                             std::span<const geometry::Rectangle> truth )
        {
            if( truth.empty() )
            {
                return 0.0;
            }
            const auto misses = std::ranges::count_if(
                truth,
                [hints]( const geometry::Rectangle& dirty_tile )
                {
                    return std::ranges::none_of(
                        hints,
                        [&dirty_tile]( const geometry::Rectangle& hint )
                        {
                            return intersects( dirty_tile, hint );
                        }
                    );
                }
            );
            return static_cast<double>( misses ) / static_cast<double>( truth.size() );
        }

    }    // namespace

    DamageHintAuditor::DamageHintAuditor( double miss_threshold ) :
        miss_threshold_{ std::clamp( miss_threshold,
                                     0.0,
                                     1.0 ) }
    {
    }

    DamageHintAudit
    DamageHintAuditor::audit( std::span<const geometry::Rectangle> hints,
                              const TileDiffResult&                ground_truth,
                              bool                                 overflowed )
    {
        if( overflowed )
        {
            degraded_           = true;
            degradation_reason_ = "damage hint queue overflowed";
            return DamageHintAudit{
                .action    = DamageHintAction::FullInvalidation,
                .miss_rate = 1.0,
                .reason    = degradation_reason_,
            };
        }
        if( ground_truth.kind == TileDiffKind::FullInvalidation )
        {
            return DamageHintAudit{
                .action    = DamageHintAction::FullInvalidation,
                .miss_rate = 0.0,
                .reason    = ground_truth.reason,
            };
        }

        const auto miss_rate = calculate_miss_rate( hints, ground_truth.dirty_tiles );
        if( !degraded_ && miss_rate > miss_threshold_ )
        {
            degraded_           = true;
            degradation_reason_ = "damage hint miss rate exceeded threshold";
        }
        if( degraded_ )
        {
            return DamageHintAudit{
                .action    = DamageHintAction::FullScan,
                .miss_rate = miss_rate,
                .reason    = degradation_reason_,
            };
        }
        return DamageHintAudit{
            .action    = DamageHintAction::UseHints,
            .miss_rate = miss_rate,
            .reason    = {},
        };
    }

    bool
    DamageHintAuditor::degraded() const noexcept
    {
        return degraded_;
    }

    std::string_view
    DamageHintAuditor::degradation_reason() const noexcept
    {
        return degradation_reason_;
    }

}    // namespace grab::kernel
