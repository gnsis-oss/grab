#pragma once

// ┌──────────────────────────────────────────────────────────────────────────┐
// │  spider/view/motion/noise.hpp — coloured noise sources for human motion  │
// └──────────────────────────────────────────────────────────────────────────┘
//
// White noise is as legible a tell as no noise at all.  Human pointer motion
// carries three distinct, separable noise processes, and a synthesiser that
// omits any of them is separable from a recording by a power-spectrum check:
//
//   * Physiological tremor — band-limited 8-12 Hz, ~0.3-0.8 px RMS.
//   * Low-frequency drift  — 1/f (pink), ~0.5-2 px over a second.
//   * Signal-dependent     — variance scaling with control-signal magnitude
//     (Harris & Wolpert 1998).  This is what makes the speed/accuracy
//     tradeoff *emerge*; Fitts's law is therefore never applied as a rule
//     anywhere in this library.
//
// See workspace/plans/2026-07-29-visual-crawl-plan.md §5A.3.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <random>

namespace ladder::view::motion
{

    // ── Deterministic PRNG ────────────────────────────────────────────────
    //
    // xoshiro256++ — seedable for replay, fast enough to call per sample.

    class Rng
    {
        public:

            explicit Rng( uint64_t seed ) noexcept
            {
                // SplitMix64 expansion of the seed into the 256-bit state.
                uint64_t z = seed;
                for( auto& word : state_ )
                {
                    z          += 0X9E'37'79'B9'7F'4A'7C'15ULL;
                    uint64_t x  = z;
                    x           = ( x ^ ( x >> 30U ) ) * 0XBF'58'47'6D'1C'E4'E5'B9ULL;
                    x           = ( x ^ ( x >> 27U ) ) * 0X94'D0'49'BB'13'31'11'EBULL;
                    word        = x ^ ( x >> 31U );
                }
            }

            [[nodiscard]]
            uint64_t
            next() noexcept
            {
                const uint64_t result  = rotl( state_[0] + state_[3], 23U ) + state_[0];
                const uint64_t t       = state_[1] << 17U;
                state_[2]             ^= state_[0];
                state_[3]             ^= state_[1];
                state_[1]             ^= state_[2];
                state_[0]             ^= state_[3];
                state_[2]             ^= t;
                state_[3]              = rotl( state_[3], 45U );
                return result;
            }

            // Uniform on [0, 1).
            [[nodiscard]]
            double
            uniform() noexcept
            {
                return static_cast<double>( next() >> 11U ) * 0X1.0P-53;    // 2^-53
            }

            [[nodiscard]]
            double
            uniform( double lo,
                     double hi ) noexcept
            {
                return lo + ( hi - lo ) * uniform();
            }

            // Box-Muller, cached second deviate.
            [[nodiscard]]
            double
            normal() noexcept
            {
                if( has_spare_ )
                {
                    has_spare_ = false;
                    return spare_;
                }
                double u = uniform();
                if( u < min_uniform )
                {
                    u = min_uniform;
                }
                const double v   = uniform();
                const double mag = std::sqrt( -2.0 * std::log( u ) );
                const double ang = 2.0 * std::numbers::pi * v;
                spare_           = mag * std::sin( ang );
                has_spare_       = true;
                return mag * std::cos( ang );
            }

            [[nodiscard]]
            double
            normal( double mean,
                    double stddev ) noexcept
            {
                return mean + ( stddev * normal() );
            }

            // Lognormal parameterised by the *log-space* mean and sigma.
            [[nodiscard]]
            double
            lognormal( double log_mean,
                       double log_sigma ) noexcept
            {
                return std::exp( normal( log_mean, log_sigma ) );
            }

            [[nodiscard]]
            bool
            chance( double probability ) noexcept
            {
                return uniform() < probability;
            }

            [[nodiscard]]
            uint64_t
            below( uint64_t bound ) noexcept
            {
                return bound == 0U ? 0U : next() % bound;
            }

        private:

            static constexpr double min_uniform = 1.0E-12;

            [[nodiscard]]
            static constexpr uint64_t
            rotl( uint64_t x,
                  unsigned k ) noexcept
            {
                return ( x << k ) | ( x >> ( 64U - k ) );
            }

            std::array<uint64_t, 4> state_{};
            double                  spare_     = 0.0;
            bool                    has_spare_ = false;
    };

    // ── Pink (1/f) noise — Voss-McCartney ─────────────────────────────────
    //
    // Models the slow wander of a resting hand.  Octave-spaced sample-and-hold
    // rows summed together give a 1/f spectrum for negligible cost.

    class PinkNoise
    {
        public:

            static constexpr std::size_t rows = 12U;

            explicit PinkNoise( Rng& rng ) :
                rng_( &rng )
            {
                for( auto& row : row_values_ )
                {
                    row = rng_->normal();
                }
                running_ = 0.0;
                for( const auto row : row_values_ )
                {
                    running_ += row;
                }
            }

            // Returns a sample with unit-ish variance; scale at the call site.
            [[nodiscard]]
            double
            next() noexcept
            {
                ++counter_;
                // Row k updates every 2^k samples — the trailing-zero count of
                // the counter names the row to refresh.
                std::size_t row = 0U;
                for( uint64_t probe = counter_; ( probe & 1U ) == 0U && row + 1U < rows;
                     probe >>= 1U )
                {
                    ++row;
                }
                running_              -= row_values_.at( row );
                row_values_.at( row )  = rng_->normal();
                running_              += row_values_.at( row );
                return running_ / std::sqrt( static_cast<double>( rows ) );
            }

        private:

            Rng*                     rng_ = nullptr;
            std::array<double, rows> row_values_{};
            double                   running_ = 0.0;
            uint64_t                 counter_ = 0U;
    };

    // ── Physiological tremor — band-limited 8-12 Hz ───────────────────────
    //
    // A 2nd-order resonant bandpass driven by white noise.  Centre frequency
    // and Q are per-session constants; amplitude drifts slowly (§5A.6).

    class TremorFilter
    {
        public:

            static constexpr double default_centre_hz = 10.0;
            static constexpr double default_q         = 3.5;

            TremorFilter( Rng&   rng,
                          double sample_rate_hz,
                          double centre_hz = default_centre_hz,
                          double q         = default_q ) :
                rng_( &rng )
            {
                // Direct-form biquad bandpass (constant peak gain).
                const double w0    = 2.0 * std::numbers::pi * centre_hz / sample_rate_hz;
                const double alpha = std::sin( w0 ) / ( 2.0 * q );
                const double a0    = 1.0 + alpha;

                b0_                = alpha / a0;
                b1_                = 0.0;
                b2_                = -alpha / a0;
                a1_                = ( -2.0 * std::cos( w0 ) ) / a0;
                a2_                = ( 1.0 - alpha ) / a0;
            }

            [[nodiscard]]
            double
            next() noexcept
            {
                const double x = rng_->normal();
                const double y = ( b0_ * x ) +
                                 ( b1_ * x1_ ) +
                                 ( b2_ * x2_ ) -
                                 ( a1_ * y1_ ) -
                                 ( a2_ * y2_ );
                x2_            = x1_;
                x1_            = x;
                y2_            = y1_;
                y1_            = y;
                // The resonator concentrates energy; normalise to ~unit RMS.
                return y * std::sqrt( default_q );
            }

        private:

            Rng*   rng_ = nullptr;
            double b0_ = 0.0, b1_ = 0.0, b2_ = 0.0, a1_ = 0.0, a2_ = 0.0;
            double x1_ = 0.0, x2_ = 0.0, y1_ = 0.0, y2_ = 0.0;
    };

    // ── Ornstein-Uhlenbeck drift — session-scale parameter wander ─────────
    //
    // Warm-up and fatigue: mean speed, tremor amplitude and correction rate
    // must not be constant across a six-hour session (§5A.6).

    class OrnsteinUhlenbeck
    {
        public:

            OrnsteinUhlenbeck( Rng&   rng,
                               double mean,
                               double reversion,
                               double volatility ) noexcept :
                rng_( &rng ),
                value_( mean ),
                mean_( mean ),
                reversion_( reversion ),
                volatility_( volatility )
            {
            }

            [[nodiscard]]
            double
            step( double dt ) noexcept
            {
                value_ += ( reversion_ * ( mean_ - value_ ) * dt ) +
                          ( volatility_ * std::sqrt( dt ) * rng_->normal() );
                return value_;
            }

            [[nodiscard]]
            double
            value() const noexcept
            {
                return value_;
            }

        private:

            Rng*   rng_        = nullptr;
            double value_      = 0.0;
            double mean_       = 0.0;
            double reversion_  = 0.0;
            double volatility_ = 0.0;
    };

}    // namespace ladder::view::motion
