#pragma once

// Clock-offset estimation from a bounded window of noisy samples.
//
// Two clocks that were never synchronised — an X server's millisecond counter
// and steady_clock, a remote peer and us — differ by an offset that a single
// observation cannot measure: scheduling delay, a preempted thread and the
// coarse resolution of the remote counter all land in that one sample. A
// single calibration point therefore carries every error that happened to be
// in flight when it was taken, and carries it for the whole session.
//
// Drift keeps the last N offset samples in a fixed std::array ring — no
// allocation, contiguous, sized at compile time — and estimates from all of
// them. Median is the default because it is the only one of the three that a
// single outlier cannot move: with N samples, half of them have to be wrong
// before the estimate is.
//
// `spread()` is the standard deviation of the same window whatever the
// strategy, which is what turns an estimate into an honest confidence
// interval rather than a number with no error bar.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <type_traits>

namespace grab::core
{

    // Estimator selection. Tags rather than an enum so the choice is made at
    // compile time and the two unselected bodies are never instantiated.
    namespace drift
    {

        // Arithmetic mean. Cheapest, and moved by every outlier.
        struct Mean
        {
        };

        // Middle sample. Order-insensitive and robust: an injected outlier
        // changes which sample is in the middle, not what the middle is.
        struct Median
        {
        };

        // Exponential moving average, alpha = 2/(N+1). Tracks a genuinely
        // moving offset faster than a window mean, at the cost of following
        // outliers part of the way.
        struct Ema
        {
        };

    }    // namespace drift

    template<std::size_t N = 16, typename Strategy = drift::Median>
    class Drift
    {
            static_assert( N > 0,
                           "Drift needs room for at least one sample" );
            static_assert( std::is_same_v<Strategy,
                                          drift::Mean> ||
                               std::is_same_v<Strategy,
                                              drift::Median> ||
                               std::is_same_v<Strategy,
                                              drift::Ema>,
                           "Drift strategy must be Mean, Median or Ema" );

        public:

            using Offset                          = std::chrono::nanoseconds;
            using Rep                             = Offset::rep;

            static constexpr std::size_t capacity = N;

            // Records one (local - remote) offset observation, evicting the
            // oldest once the window is full.
            void
            feed( Offset offset ) noexcept
            {
                samples_[next_] = offset;
                next_           = ( next_ + 1 ) % N;
                filled_         = std::min( filled_ + 1, N );
            }

            // The offset estimate under the selected strategy. Zero while no
            // sample has been fed — a caller that must distinguish "no offset"
            // from "an offset of zero" reads samples().
            [[nodiscard]]
            Offset
            shift() const noexcept
            {
                if( filled_ == 0 )
                {
                    return Offset::zero();
                }

                if constexpr( std::is_same_v<Strategy, drift::Mean> )
                {
                    return mean_offset();
                }
                else if constexpr( std::is_same_v<Strategy, drift::Median> )
                {
                    return median_offset();
                }
                else
                {
                    return ema_offset();
                }
            }

            // Population standard deviation of the window, whatever the
            // strategy. Zero below two samples: a single point has no spread,
            // and reporting one would be an invented confidence interval.
            [[nodiscard]]
            Offset
            spread() const noexcept
            {
                if( filled_ < 2 )
                {
                    return Offset::zero();
                }

                const double mean     = mean_value();
                double       variance = 0.0;
                for( std::size_t index = 0; index < filled_; ++index )
                {
                    const double deviation =
                        static_cast<double>( samples_[index].count() ) - mean;
                    variance += deviation * deviation;
                }
                variance /= static_cast<double>( filled_ );

                return Offset{
                    static_cast<Rep>( std::llround( std::sqrt( variance ) ) )
                };
            }

            // Samples currently in the window, saturating at N.
            [[nodiscard]]
            std::size_t
            samples() const noexcept
            {
                return filled_;
            }

        private:

            std::array<Offset, N> samples_{};
            std::size_t           next_   = 0;
            std::size_t           filled_ = 0;

            // Index of the oldest sample. Before the first wrap the ring is a
            // plain prefix and the oldest is at 0; after it, next_ is the slot
            // about to be overwritten, which is exactly the oldest.
            [[nodiscard]]
            std::size_t
            oldest() const noexcept
            {
                return filled_ == N ? next_ : 0U;
            }

            [[nodiscard]]
            double
            mean_value() const noexcept
            {
                Rep sum = 0;
                for( std::size_t index = 0; index < filled_; ++index )
                {
                    sum += samples_[index].count();
                }
                return static_cast<double>( sum ) / static_cast<double>( filled_ );
            }

            [[nodiscard]]
            Offset
            mean_offset() const noexcept
            {
                return Offset{ static_cast<Rep>( std::llround( mean_value() ) ) };
            }

            // nth_element over a copy of the filled prefix: O(N) rather than
            // O(N log N), and it leaves the ring untouched. Storage order is
            // irrelevant here — a median does not care how its inputs arrived.
            [[nodiscard]]
            Offset
            median_offset() const noexcept
            {
                std::array<Rep, N> ordered{};
                for( std::size_t index = 0; index < filled_; ++index )
                {
                    ordered[index] = samples_[index].count();
                }

                const auto begin = ordered.begin();
                const auto end =
                    std::next( begin, static_cast<std::ptrdiff_t>( filled_ ) );
                const auto middle =
                    std::next( begin, static_cast<std::ptrdiff_t>( filled_ / 2U ) );

                std::nth_element( begin, middle, end );
                const Rep upper = *middle;
                if( filled_ % 2U == 1U )
                {
                    return Offset{ upper };
                }

                // Even count: average the two central samples. nth_element has
                // already partitioned everything below `middle`, so the lower
                // central sample is the largest of that part. Written as
                // low + (high - low)/2 rather than (low + high)/2 so a pair of
                // large offsets cannot overflow on the way to their midpoint.
                const Rep lower = *std::max_element( begin, middle );
                return Offset{ lower + ( ( upper - lower ) / 2 ) };
            }

            // An EMA is order-sensitive, so it must walk *insertion* order.
            // Storage order diverges from insertion order the moment the ring
            // wraps, and a naive index loop over a wrapped ring weights the
            // oldest sample most heavily — the exact inversion of what an EMA
            // is for. Walking from oldest() keeps the recency weighting right
            // across every wrap.
            [[nodiscard]]
            Offset
            ema_offset() const noexcept
            {
                const std::size_t start = oldest();
                double            value = 0.0;
                for( std::size_t step = 0; step < filled_; ++step )
                {
                    const double sample =
                        static_cast<double>( samples_[( start + step ) % N].count() );
                    value = step == 0 ? sample
                                      : ( alpha * sample ) + ( ( 1.0 - alpha ) * value );
                }
                return Offset{ static_cast<Rep>( std::llround( value ) ) };
            }

            static constexpr double alpha = 2.0 / ( static_cast<double>( N ) + 1.0 );
    };

}    // namespace grab::core
