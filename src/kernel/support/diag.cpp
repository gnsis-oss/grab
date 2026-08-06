#include "kernel/support/diag.hpp"
#include "kernel/support/log.hpp"
#include "kernel/support/log_tags.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <optional>
#include <ratio>

namespace grab::diag
{
    namespace
    {

        // A frame budget of 16.7 ms, matching the overlay's 60 FPS governor.
        constexpr auto   frameBudget = std::chrono::nanoseconds{ 16'666'667 };

        // An event that resolves this far from now, in either direction, is
        // not plausibly the input this frame reflects — treat the correlation
        // as stale rather than reporting a nonsense latency.
        constexpr auto   maximumServerSkew = std::chrono::milliseconds{ 60'000 };

        constexpr double p50Fraction       = 0.50;
        constexpr double p95Fraction       = 0.95;

        struct Store
        {
                std::array<FrameSample, frameWindow> window{};
                std::size_t                          next    = 0;
                std::size_t                          filled  = 0;
                std::size_t                          total   = 0;
                std::uint64_t                        pixels  = 0;
                std::uint64_t                        events  = 0;
                std::size_t                          overrun = 0;
        };

        std::mutex&
        store_mutex() noexcept
        {
            static std::mutex mutex;
            return mutex;
        }

        Store&
        store() noexcept
        {
            static Store state;
            return state;
        }

        // Scratch for quantile extraction. Under the same lock as the store,
        // so one buffer serves every field and nothing is allocated on the
        // reporting path.
        std::array<std::chrono::nanoseconds::rep,
                   frameWindow>&
        scratch() noexcept
        {
            static std::array<std::chrono::nanoseconds::rep, frameWindow> buffer{};
            return buffer;
        }

        [[nodiscard]]
        std::chrono::nanoseconds
        at_fraction( std::size_t count,
                     double      fraction ) noexcept
        {
            if( count == 0 )
            {
                return std::chrono::nanoseconds::zero();
            }
            auto index =
                static_cast<std::size_t>( fraction * static_cast<double>( count - 1 ) );
            index = std::min( index, count - 1 );
            return std::chrono::nanoseconds{ scratch()[index] };
        }

        // Extracts one field from the window into the shared scratch, sorts
        // it, and reads the four order statistics off it.
        template<typename Project>
        [[nodiscard]]
        Quantiles
        quantiles_of( std::size_t count,
                      Project     project ) noexcept
        {
            if( count == 0 )
            {
                return {};
            }
            const auto& state = store();
            for( std::size_t index = 0; index < count; ++index )
            {
                scratch()[index] = project( state.window[index] ).count();
            }
            std::sort( scratch().begin(),
                       std::next( scratch().begin(),
                                  static_cast<std::ptrdiff_t>( count ) ) );
            return Quantiles{
                .min = std::chrono::nanoseconds{ scratch()[0] },
                .p50 = at_fraction( count, p50Fraction ),
                .p95 = at_fraction( count, p95Fraction ),
                .max = std::chrono::nanoseconds{ scratch()[count - 1] },
            };
        }

        // Difference between two readings of the server's 32-bit millisecond
        // counter, as a signed span. Unsigned subtraction wraps exactly as the
        // counter does, so casting the wrapped difference to int32 recovers a
        // forward *or* backward delta of up to ±24.8 days with no rollover
        // special case — and without mistaking an event 5 ms in the past for
        // one 49.7 days in the future.
        [[nodiscard]]
        std::int64_t
        server_step_ms( std::uint32_t from,
                        std::uint32_t to ) noexcept
        {
            return static_cast<std::int32_t>( to - from );
        }

        // maybe_unused because at a compile ceiling of Off every emitter lambda
        // in log_report is discarded, leaving this with no caller — which is
        // the intended outcome, not a mistake.
        [[nodiscard,
          maybe_unused]]
        double
        to_ms( std::chrono::nanoseconds value ) noexcept
        {
            return std::chrono::duration<double, std::milli>( value ).count();
        }

    }    // namespace

    // ── ServerClock ────────────────────────────────────────

    void
    ServerClock::calibrate( std::uint32_t                         server_ms,
                            std::chrono::steady_clock::time_point observed_at ) noexcept
    {
        if( !calibrated_ )
        {
            origin_            = observed_at;
            elapsed_server_ms_ = 0;
            last_server_ms_    = server_ms;
            calibrated_        = true;
            offsets_.feed( std::chrono::nanoseconds::zero() );
            return;
        }

        elapsed_server_ms_ += server_step_ms( last_server_ms_, server_ms );
        last_server_ms_     = server_ms;

        // The residual this sample contributes: how far the local clock has
        // moved beyond the server's own account of the same interval. A wild
        // sample distorts one residual and nothing else — the next sample's
        // step subtracts the same error back out, and the median discards the
        // one that was wrong.
        const auto residual =
            ( observed_at - origin_ ) - std::chrono::milliseconds{ elapsed_server_ms_ };
        offsets_.feed(
            std::chrono::duration_cast<std::chrono::nanoseconds>( residual )
        );
    }

    std::optional<std::chrono::steady_clock::time_point>
    ServerClock::instant_of( std::uint32_t server_ms ) const noexcept
    {
        if( !calibrated_ )
        {
            return std::nullopt;
        }

        const auto elapsed_ms =
            elapsed_server_ms_ + server_step_ms( last_server_ms_, server_ms );
        const auto instant =
            origin_ + std::chrono::milliseconds{ elapsed_ms } + offsets_.shift();

        // Staleness is a question about the local clock, not about the
        // calibration point: an event is worth reporting a latency for when it
        // resolves to an instant near now. Measuring it from the first sample
        // instead would make a session's own age the disqualifier, so that
        // every event past minute one resolved to nothing.
        const auto now  = std::chrono::steady_clock::now();
        const auto skew = instant > now ? instant - now : now - instant;
        if( skew > maximumServerSkew )
        {
            return std::nullopt;
        }
        return instant;
    }

    // ── Samples ────────────────────────────────────────────

    void
    record_frame( const FrameSample& sample ) noexcept
    {
        const std::scoped_lock lock{ store_mutex() };
        auto&                  state = store();

        state.window[state.next]     = sample;
        state.next                   = ( state.next + 1 ) % frameWindow;
        state.filled                 = std::min( state.filled + 1, frameWindow );
        ++state.total;
        state.pixels += sample.damaged_pixels;
        state.events += sample.events_drained;

        const auto spent =
            sample.raster + sample.convert + sample.present + sample.flush;
        if( spent > frameBudget )
        {
            ++state.overrun;
        }
    }

    FrameReport
    report() noexcept
    {
        const std::scoped_lock lock{ store_mutex() };
        const auto&            state = store();
        const auto             count = state.filled;

        return FrameReport{
            .frames           = count,
            .total            = state.total,
            .raster           = quantiles_of( count,
                                              []( const FrameSample& s )
                                              {
                                        return s.raster;
                                              } ),
            .convert          = quantiles_of( count,
                                              []( const FrameSample& s )
                                              {
                                         return s.convert;
                                              } ),
            .present          = quantiles_of( count,
                                              []( const FrameSample& s )
                                              {
                                         return s.present;
                                              } ),
            .flush            = quantiles_of( count,
                                              []( const FrameSample& s )
                                              {
                                       return s.flush;
                                              } ),
            .input_to_present = quantiles_of( count,
                                              []( const FrameSample& s )
                                              {
                                                  return s.input_to_present;
                                              } ),
            .over_budget      = state.overrun,
            .damaged_pixels   = state.pixels,
            .events_drained   = state.events,
        };
    }

    void
    reset() noexcept
    {
        const std::scoped_lock lock{ store_mutex() };
        store() = Store{};
    }

    bool
    due_for_report() noexcept
    {
        const std::scoped_lock lock{ store_mutex() };
        const auto&            state = store();
        return state.total > 0 && ( state.total % reportCadence ) == 0;
    }

    void
    log_report( const FrameReport& summary,
                log::Level         level ) noexcept
    {
        const auto emit = [&summary]( auto& event )
        {
            event.tag( log::tags::frame )
                .value( "frames", summary.frames )
                .value( "total", summary.total )
                .value( "over_budget", summary.over_budget )
                .value( "raster_p50_ms", to_ms( summary.raster.p50 ) )
                .value( "raster_p95_ms", to_ms( summary.raster.p95 ) )
                .value( "convert_p50_ms", to_ms( summary.convert.p50 ) )
                .value( "convert_p95_ms", to_ms( summary.convert.p95 ) )
                .value( "present_p50_ms", to_ms( summary.present.p50 ) )
                .value( "present_p95_ms", to_ms( summary.present.p95 ) )
                .value( "flush_p50_ms", to_ms( summary.flush.p50 ) )
                .value( "flush_p95_ms", to_ms( summary.flush.p95 ) )
                .value( "latency_p50_ms", to_ms( summary.input_to_present.p50 ) )
                .value( "latency_p95_ms", to_ms( summary.input_to_present.p95 ) )
                .value( "latency_max_ms", to_ms( summary.input_to_present.max ) )
                .value( "damaged_pixels", summary.damaged_pixels )
                .value( "events", summary.events_drained );
        };

        switch( level )
        {
            case log::Level::Nominal :
                log::nominal( emit );
                return;
            case log::Level::Verbose :
                log::verbose( emit );
                return;
            case log::Level::Debug :
                log::debug( emit );
                return;
            case log::Level::Off :
                return;
        }
    }

}    // namespace grab::diag
