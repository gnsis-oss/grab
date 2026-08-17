#pragma once

// ┌──────────────────────────────────────────────────────────────────────────┐
// │  spider/view/motion/trajectory.hpp — full human pointer trajectories    │
// └──────────────────────────────────────────────────────────────────────────┘
//
// Assembles the pieces into an emittable pointer track:
//
//   1. Meyer's stochastic optimized-submovement model — the number of
//      corrective submovements is itself a random variable, not a fixed
//      overshoot rule.  Mostly 0-1 corrections, occasionally 3.
//   2. Signal-dependent endpoint noise (Harris & Wolpert) — variance scales
//      with movement amplitude, so fast segments miss and slow corrective
//      ones do not.  The speed/accuracy tradeoff emerges; Fitts's law is
//      never applied as an explicit rule.
//   3. Superimposed tremor (8-12 Hz) and 1/f drift.
//   4. Device-plausible emission — jittered report intervals drawn from a
//      distribution, sub-pixel residual carried forward the way a real
//      driver does, and occasional coalesced samples.
//
// See workspace/plans/2026-07-29-visual-crawl-plan.md §5A.4-§5A.5.

#include "support/motion/noise.hpp"
#include "support/motion/sigma.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ladder::view::motion
{

    // ── Emitted sample ────────────────────────────────────────────────────

    struct Sample
    {
            double t_s_ = 0.0;    // seconds from movement start
            int    x_   = 0;      // integer screen pixels
            int    y_   = 0;
    };

    struct Rect
    {
            double x_ = 0.0;
            double y_ = 0.0;
            double w_ = 0.0;
            double h_ = 0.0;

            [[nodiscard]]
            bool
            contains( Vec2 point ) const noexcept
            {
                const bool inside_x = point.x_ >= x_ && point.x_ <= ( x_ + w_ );
                const bool inside_y = point.y_ >= y_ && point.y_ <= ( y_ + h_ );
                return inside_x && inside_y;
            }

            [[nodiscard]]
            Vec2
            centre() const noexcept
            {
                return Vec2{ x_ + ( w_ / 2.0 ), y_ + ( h_ / 2.0 ) };
            }
    };

    // ── Tuning ────────────────────────────────────────────────────────────

    struct MotionConfig
    {
            // Emission — 125 Hz mouse, jittered.  A *constant* interval is
            // grab::input::DragOptions' tell and must never be reproduced.
            double       report_interval_mean_ = 0.008;
            double       report_interval_std_  = 0.0017;
            double       coalesce_probability_ = 0.04;

            // Integration resolution (well below the report interval).
            double       integration_dt_ = 0.001;

            // Signal-dependent endpoint noise: sd = fraction * amplitude.
            double       endpoint_noise_frac_ = 0.055;

            // Corrective submovements.
            std::size_t  max_submovements_            = 4U;
            double       correction_latency_log_mean_ = -2.41;    // ~90 ms
            double       correction_latency_log_std_  = 0.30;

            // Superimposed noise amplitudes, in pixels.
            double       tremor_px_ = 0.45;
            double       drift_px_  = 0.85;

            // Reaction latency before the first sample moves at all.
            double       reaction_log_mean_ = -1.51;    // ~220 ms
            double       reaction_log_std_  = 0.40;

            StrokeParams stroke_{};
    };

    // ── Click-point sampling ──────────────────────────────────────────────
    //
    // Never the element centre.  Truncated normal over the rect, with the
    // mean pulled slightly toward the approach direction: you land short of
    // where you aimed and click where you landed.

    [[nodiscard]]
    inline Vec2
    sample_click_point( Rng&        rng,
                        const Rect& target,
                        Vec2        approach_from )
    {
        constexpr double spread_frac   = 0.22;
        constexpr double approach_bias = 0.08;
        constexpr double edge_inset_px = 3.0;

        const Vec2       centre        = target.centre();
        const double     dx            = centre.x_ - approach_from.x_;
        const double     dy            = centre.y_ - approach_from.y_;
        const double     norm          = std::hypot( dx, dy );

        Vec2             mean          = centre;
        if( norm > 0.0 )
        {
            // Bias *against* the approach direction — short of centre.
            mean.x_ -= ( dx / norm ) * target.w_ * approach_bias;
            mean.y_ -= ( dy / norm ) * target.h_ * approach_bias;
        }

        const double lo_x = target.x_ + edge_inset_px;
        const double hi_x = target.x_ + target.w_ - edge_inset_px;
        const double lo_y = target.y_ + edge_inset_px;
        const double hi_y = target.y_ + target.h_ - edge_inset_px;

        Vec2         point{
            rng.normal( mean.x_, std::max( 1.0, target.w_ * spread_frac ) ),
            rng.normal( mean.y_, std::max( 1.0, target.h_ * spread_frac ) ),
        };
        point.x_ =
            std::clamp( point.x_, std::min( lo_x, hi_x ), std::max( lo_x, hi_x ) );
        point.y_ =
            std::clamp( point.y_, std::min( lo_y, hi_y ), std::max( lo_y, hi_y ) );
        return point;
    }

    // ── One submovement ───────────────────────────────────────────────────
    //
    // Produces a continuous position track from `origin` toward `aim`, landing
    // at aim + signal-dependent noise.  Shape comes from the sigma-lognormal
    // superposition; the affine fit preserves that shape while pinning the
    // endpoint.

    [[nodiscard]]
    inline std::vector<Vec2>
    submovement( Rng&                rng,
                 Vec2                origin,
                 Vec2                aim,
                 const MotionConfig& config )
    {
        const double dx        = aim.x_ - origin.x_;
        const double dy        = aim.y_ - origin.y_;
        const double amplitude = std::hypot( dx, dy );
        if( amplitude < 0.5 )
        {
            return {};
        }
        const double      chord = std::atan2( dy, dx );

        // Longer movements recruit more overlapping strokes.
        const std::size_t stroke_count =
            amplitude > 400.0 ? 3U : ( amplitude > 120.0 ? 2U : 1U );

        std::vector<Stroke> strokes;
        strokes.reserve( stroke_count );
        double onset = 0.0;
        for( std::size_t index = 0U; index < stroke_count; ++index )
        {
            const double share = amplitude / static_cast<double>( stroke_count );
            strokes.push_back(
                sample_stroke( rng, share, chord, onset, config.stroke_ )
            );
            // Strokes overlap — the next activates before the previous ends.
            onset += rng.lognormal( -2.9, 0.35 );
        }

        const double window = integration_window( strokes );
        auto         track  = integrate( strokes, window, config.integration_dt_ );
        if( track.empty() )
        {
            return track;
        }

        // Signal-dependent endpoint noise (Harris & Wolpert): sd scales with
        // amplitude, so this is where misses come from.
        const double noise_sd = config.endpoint_noise_frac_ * amplitude;
        const Vec2   landing{
            aim.x_ + rng.normal( 0.0, noise_sd ),
            aim.y_ + rng.normal( 0.0, noise_sd ),
        };

        // Affine (scale + rotate) fit of the integrated shape onto the
        // required displacement.  Preserves curvature and velocity structure.
        const Vec2   raw_end = track.back();
        const double raw_mag = std::hypot( raw_end.x_, raw_end.y_ );
        if( raw_mag < 1.0E-6 )
        {
            return {};
        }
        const double want_dx  = landing.x_ - origin.x_;
        const double want_dy  = landing.y_ - origin.y_;
        const double want_mag = std::hypot( want_dx, want_dy );
        const double scale    = want_mag / raw_mag;
        const double rotate =
            std::atan2( want_dy, want_dx ) - std::atan2( raw_end.y_, raw_end.x_ );
        const double cos_r = std::cos( rotate );
        const double sin_r = std::sin( rotate );

        for( auto& point : track )
        {
            const double rx = ( point.x_ * cos_r ) - ( point.y_ * sin_r );
            const double ry = ( point.x_ * sin_r ) + ( point.y_ * cos_r );
            point.x_        = origin.x_ + ( rx * scale );
            point.y_        = origin.y_ + ( ry * scale );
        }
        return track;
    }

    // ── Full movement: primary + stochastic corrections ───────────────────

    struct Movement
    {
            std::vector<Sample> samples_;
            Vec2                landed_{};
            std::size_t         corrections_ = 0U;
            double              duration_s_  = 0.0;
    };

    [[nodiscard]]
    inline Movement
    plan_move( Rng&                rng,
               Vec2                from,
               const Rect&         target,
               const MotionConfig& config )
    {
        const Vec2          aim = sample_click_point( rng, target, from );

        // Continuous track at integration resolution, assembled across
        // however many submovements it takes.
        std::vector<Vec2>   path;
        std::vector<double> path_time;

        double              cursor_t =
            rng.lognormal( config.reaction_log_mean_, config.reaction_log_std_ );
        Vec2        current     = from;
        std::size_t corrections = 0U;

        for( std::size_t attempt = 0U; attempt < config.max_submovements_; ++attempt )
        {
            const auto track = submovement( rng, current, aim, config );
            if( track.empty() )
            {
                break;
            }
            for( std::size_t index = 0U; index < track.size(); ++index )
            {
                path.push_back( track.at( index ) );
                path_time.push_back( cursor_t + ( static_cast<double>( index ) *
                                                  config.integration_dt_ ) );
            }
            cursor_t += static_cast<double>( track.size() ) * config.integration_dt_;
            current   = track.back();

            // Landed inside the target?  Then no correction is issued — this
            // is why the correction count is a random variable.
            if( target.contains( current ) )
            {
                break;
            }
            ++corrections;
            cursor_t += rng.lognormal( config.correction_latency_log_mean_,
                                       config.correction_latency_log_std_ );
        }

        if( path.empty() )
        {
            return Movement{ .samples_ = {}, .landed_ = from, .corrections_ = 0U };
        }

        // ── Superimpose tremor + 1/f drift ────────────────────────────────
        const double sample_rate = 1.0 / config.integration_dt_;
        TremorFilter tremor_x( rng, sample_rate );
        TremorFilter tremor_y( rng, sample_rate );
        PinkNoise    drift_x( rng );
        PinkNoise    drift_y( rng );

        for( auto& point : path )
        {
            point.x_ += ( tremor_x.next() * config.tremor_px_ ) +
                        ( drift_x.next() * config.drift_px_ );
            point.y_ += ( tremor_y.next() * config.tremor_px_ ) +
                        ( drift_y.next() * config.drift_px_ );
        }

        // ── Emit at jittered device intervals with sub-pixel carry ────────
        //
        // Rounding each sample independently would destroy the velocity
        // spectrum even with a perfect underlying model, so the residual is
        // carried forward exactly as a real driver accumulates it.

        Movement movement;
        movement.corrections_ = corrections;

        double       carry_x  = 0.0;
        double       carry_y  = 0.0;
        double       emit_t   = path_time.front();
        const double end      = path_time.back();
        std::size_t  cursor   = 0U;

        while( emit_t <= end )
        {
            while(
                cursor + 1U < path_time.size() && path_time.at( cursor + 1U ) < emit_t
            )
            {
                ++cursor;
            }
            // Linear interpolation between integration samples.
            Vec2 point = path.at( cursor );
            if( cursor + 1U < path.size() )
            {
                const double span = path_time.at( cursor + 1U ) - path_time.at( cursor );
                if( span > 0.0 )
                {
                    const double frac  = ( emit_t - path_time.at( cursor ) ) / span;
                    point.x_          += ( path.at( cursor + 1U ).x_ - point.x_ ) * frac;
                    point.y_          += ( path.at( cursor + 1U ).y_ - point.y_ ) * frac;
                }
            }

            const double want_x = point.x_ + carry_x;
            const double want_y = point.y_ + carry_y;
            const auto   out_x  = static_cast<int>( std::lround( want_x ) );
            const auto   out_y  = static_cast<int>( std::lround( want_y ) );
            carry_x             = want_x - static_cast<double>( out_x );
            carry_y             = want_y - static_cast<double>( out_y );

            // Occasional coalesced sample — X compresses motion under load.
            if( !rng.chance( config.coalesce_probability_ ) )
            {
                movement.samples_.push_back( Sample{ emit_t, out_x, out_y } );
            }

            emit_t += std::max( 0.001,
                                rng.normal( config.report_interval_mean_,
                                            config.report_interval_std_ ) );
        }

        if( movement.samples_.empty() )
        {
            movement.samples_.push_back( Sample{
                emit_t,
                static_cast<int>( std::lround( path.back().x_ ) ),
                static_cast<int>( std::lround( path.back().y_ ) )
            } );
        }

        // The final sample must land exactly where we intend to click.
        movement.samples_.back().x_ = static_cast<int>( std::lround( current.x_ ) );
        movement.samples_.back().y_ = static_cast<int>( std::lround( current.y_ ) );
        movement.landed_            = current;
        movement.duration_s_        = movement.samples_.back().t_s_;
        return movement;
    }

    // ── Pre-click pause and button hold ───────────────────────────────────
    //
    // Nobody clicks the instant they land, and nobody holds the button for a
    // constant time.

    [[nodiscard]]
    inline double
    sample_pre_click_pause( Rng& rng ) noexcept
    {
        constexpr double log_mean = -2.66;    // ~70 ms
        constexpr double log_std  = 0.45;
        return rng.lognormal( log_mean, log_std );
    }

    [[nodiscard]]
    inline double
    sample_button_hold( Rng& rng ) noexcept
    {
        constexpr double log_mean = -2.53;    // ~80 ms
        constexpr double log_std  = 0.35;
        return rng.lognormal( log_mean, log_std );
    }

}    // namespace ladder::view::motion
