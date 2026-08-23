#pragma once

// ┌──────────────────────────────────────────────────────────────────────────┐
// │  spider/view/motion/sigma.hpp — Sigma-Lognormal movement synthesis      │
// └──────────────────────────────────────────────────────────────────────────┘
//
// The Kinematic Theory of Rapid Human Movements (Plamondon).  A movement is a
// *vectorial superposition of time-shifted lognormal strokes*:
//
//                    D_i              ⎡  (ln(t - t0_i) - mu_i)²  ⎤
//     v_i(t) = ───────────────────  exp ⎢ - ─────────────────────── ⎥
//              sigma_i √(2π)(t - t0_i)   ⎣          2 sigma_i²        ⎦
//
//     phi_i(t) = theta_s_i + (theta_e_i - theta_s_i) · Lambda_i(t)
//
// where Lambda_i is the stroke's normalised lognormal CDF.  The lognormal is
// *asymmetric* — fast rise, long tail — which is what real velocity profiles
// look like and precisely what a symmetric minimum-jerk bell gets wrong.
// Overlapping strokes produce the irregular, multi-peaked velocity traces
// humans actually generate.
//
// This is the Layer-2 fallback of §5A: used when the recorded-motion corpus
// has no near neighbour for the required (distance, angle, target width).
//
// See workspace/plans/2026-07-29-visual-crawl-plan.md §5A.2.

#include "support/motion/noise.hpp"

#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

namespace ladder::view::motion
{

    struct Vec2
    {
            double x_ = 0.0;
            double y_ = 0.0;
    };

    // ── One lognormal stroke ──────────────────────────────────────────────

    struct Stroke
    {
            double amplitude_ = 0.0;    // D   — stroke length in px
            double onset_     = 0.0;    // t0  — activation time, seconds
            double log_time_  = 0.0;    // mu  — log-time centre
            double log_sigma_ = 0.0;    // sigma
            double angle_in_  = 0.0;    // theta_s — direction at stroke start
            double angle_out_ = 0.0;    // theta_e — direction at stroke end
    };

    // ── Lognormal helpers ─────────────────────────────────────────────────

    [[nodiscard]]
    inline double
    stroke_speed( const Stroke& stroke,
                  double        t ) noexcept
    {
        const double dt = t - stroke.onset_;
        if( dt <= 0.0 )
        {
            return 0.0;
        }
        const double z = ( std::log( dt ) - stroke.log_time_ ) / stroke.log_sigma_;
        const double denom =
            stroke.log_sigma_ * std::sqrt( 2.0 * std::numbers::pi ) * dt;
        return ( stroke.amplitude_ / denom ) * std::exp( -0.5 * z * z );
    }

    // Normalised lognormal CDF — drives the direction interpolation.
    [[nodiscard]]
    inline double
    stroke_progress( const Stroke& stroke,
                     double        t ) noexcept
    {
        const double dt = t - stroke.onset_;
        if( dt <= 0.0 )
        {
            return 0.0;
        }
        const double z = ( std::log( dt ) - stroke.log_time_ ) /
                         ( stroke.log_sigma_ * std::numbers::sqrt2 );
        return 0.5 * ( 1.0 + std::erf( z ) );
    }

    [[nodiscard]]
    inline Vec2
    stroke_velocity( const Stroke& stroke,
                     double        t ) noexcept
    {
        const double speed = stroke_speed( stroke, t );
        if( speed <= 0.0 )
        {
            return Vec2{};
        }
        const double phi =
            stroke.angle_in_ +
            ( ( stroke.angle_out_ - stroke.angle_in_ ) * stroke_progress( stroke, t ) );
        return Vec2{ speed * std::cos( phi ), speed * std::sin( phi ) };
    }

    // ── Stroke sampling ───────────────────────────────────────────────────
    //
    // Parameter ranges follow the Kinematic Theory literature for rapid aimed
    // movements.  Every value is drawn per stroke; nothing here is constant.

    struct StrokeParams
    {
            // Log-time centre: controls when the velocity peak occurs.
            double log_time_mean_ = -1.55;
            double log_time_std_  = 0.22;
            // Log-space spread: controls profile asymmetry / tail length.
            double log_sigma_mean_ = 0.28;
            double log_sigma_std_  = 0.07;
            // Angular deviation of the stroke arc from the straight chord.
            double arc_std_ = 0.16;
    };

    [[nodiscard]]
    inline Stroke
    sample_stroke( Rng&                rng,
                   double              amplitude,
                   double              chord_angle,
                   double              onset,
                   const StrokeParams& params )
    {
        // Humans arc; the entry and exit directions differ and neither equals
        // the chord.  Both are drawn independently around it.
        const double arc_in  = rng.normal( 0.0, params.arc_std_ );
        const double arc_out = rng.normal( 0.0, params.arc_std_ * 0.6 );

        return Stroke{
            .amplitude_ = amplitude,
            .onset_     = onset,
            .log_time_  = rng.normal( params.log_time_mean_, params.log_time_std_ ),
            .log_sigma_ =
                std::max( 0.05,
                          rng.normal( params.log_sigma_mean_, params.log_sigma_std_ ) ),
            .angle_in_  = chord_angle + arc_in,
            .angle_out_ = chord_angle + arc_out,
        };
    }

    // ── Superposition ─────────────────────────────────────────────────────
    //
    // Integrates the summed velocity field of every stroke to a position
    // track.  `dt` should be well below the emission interval so the
    // integration error stays under a fraction of a pixel.

    [[nodiscard]]
    inline std::vector<Vec2>
    integrate( const std::vector<Stroke>& strokes,
               double                     duration,
               double                     dt )
    {
        std::vector<Vec2> track;
        if( duration <= 0.0 || dt <= 0.0 )
        {
            return track;
        }
        const auto steps = static_cast<std::size_t>( duration / dt ) + 1U;
        track.reserve( steps );

        Vec2 position{};
        for( std::size_t step = 0U; step < steps; ++step )
        {
            const double t = static_cast<double>( step ) * dt;
            Vec2         velocity{};
            for( const auto& stroke : strokes )
            {
                const Vec2 contribution  = stroke_velocity( stroke, t );
                velocity.x_             += contribution.x_;
                velocity.y_             += contribution.y_;
            }
            position.x_ += velocity.x_ * dt;
            position.y_ += velocity.y_ * dt;
            track.push_back( position );
        }
        return track;
    }

    // ── Duration heuristic ────────────────────────────────────────────────
    //
    // How long to integrate so the lognormal tails have effectively closed.
    // Not a movement-time law — the *shape* of the profile determines the
    // real duration; this only bounds the integration window.

    [[nodiscard]]
    inline double
    integration_window( const std::vector<Stroke>& strokes ) noexcept
    {
        double window = 0.0;
        for( const auto& stroke : strokes )
        {
            // Lognormal mass is essentially exhausted by exp(mu + 3.5 sigma).
            const double tail = stroke.onset_ + std::exp( stroke.log_time_ +
                                                          ( 3.5 * stroke.log_sigma_ ) );
            window            = std::max( window, tail );
        }
        return window;
    }

}    // namespace ladder::view::motion
