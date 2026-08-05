#pragma once

// Frame-pipeline instrumentation.
//
// Logging tells you what happened; this tells you how long it took. The
// overlay runs on a 60 FPS pacing governor — a 16.7 ms budget — and without
// per-phase numbers "the overlay is laggy" cannot be turned into a decision
// about what to change.
//
// Samples are NOT written to the log per frame: a write(2) from the reactor
// thread every 16.7 ms would perturb the very latency being measured. They
// accumulate in memory and are reported on a slow cadence.

#include "kernel/support/log.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

namespace grab::diag
{

    // How many recent frames the report summarises.
    inline constexpr std::size_t frameWindow = 512U;

    // Frames between periodic reports. At 60 FPS this is roughly every two
    // seconds — often enough to watch a trend, rare enough that the report
    // itself is not part of the measurement.
    inline constexpr std::size_t reportCadence = 120U;

    // ── Scope ──────────────────────────────────────────────

    // RAII phase timer. When `level` is compiled out the clock reads vanish
    // and the object is empty — `sizeof(Scope<compiled_out>) == 1` and its
    // constructor and destructor generate no code.
    template<log::Level level>
    class Scope
    {
            using Clock = std::chrono::steady_clock;

            struct Idle
            {
            };

            using Store =
                std::conditional_t<log::enabled( level ), Clock::time_point, Idle>;

            [[no_unique_address]]
            Store start_{};

        public:

            Scope() noexcept
            {
                if constexpr( log::enabled( level ) )
                {
                    start_ = Clock::now();
                }
            }

            [[nodiscard]]
            std::chrono::nanoseconds
            elapsed() const noexcept
            {
                if constexpr( log::enabled( level ) )
                {
                    return Clock::now() - start_;
                }
                else
                {
                    return std::chrono::nanoseconds::zero();
                }
            }
    };

    // ── Server clock ───────────────────────────────────────

    // An XInput2 event carries an X server timestamp: milliseconds since the
    // server started, in a 32-bit counter that wraps roughly every 49.7 days.
    // It shares no origin with steady_clock, so subtracting one from the other
    // is meaningless. This ties the two domains together by sampling both at a
    // known instant, which is the only way input-to-present latency means
    // anything.
    class ServerClock
    {
        public:

            // Record that `server_ms` was the server's time approximately now.
            void
            calibrate( std::uint32_t server_ms ) noexcept;

            // Steady-clock instant corresponding to an event's server time,
            // or nullopt when uncalibrated or when the offset looks stale
            // (the event predates the calibration point, or is implausibly
            // far ahead of it).
            [[nodiscard]]
            std::optional<std::chrono::steady_clock::time_point>
            instant_of( std::uint32_t server_ms ) const noexcept;

            [[nodiscard]]
            bool
            calibrated() const noexcept
            {
                return calibrated_;
            }

        private:

            std::chrono::steady_clock::time_point base_steady_{};
            std::uint32_t                         base_server_ms_ = 0;
            bool                                  calibrated_     = false;
    };

    // ── Samples ────────────────────────────────────────────

    struct FrameSample
    {
            // The four phases of X11OverlayDelegate::present_tick().
            std::chrono::nanoseconds raster{};
            std::chrono::nanoseconds convert{};
            std::chrono::nanoseconds present{};
            std::chrono::nanoseconds flush{};

            // Server-timestamp of the newest input that this frame reflects,
            // to the moment it was flushed. Zero when uncalibrated or when the
            // frame reflects no input. Resolution is 1 ms — the X server's.
            std::chrono::nanoseconds input_to_present{};

            std::uint32_t            damaged_pixels = 0;
            std::uint32_t            events_drained = 0;
    };

    struct Quantiles
    {
            std::chrono::nanoseconds min{};
            std::chrono::nanoseconds p50{};
            std::chrono::nanoseconds p95{};
            std::chrono::nanoseconds max{};
    };

    struct FrameReport
    {
            std::size_t   frames = 0;    // samples in the window
            std::size_t   total  = 0;    // frames recorded since the last reset

            Quantiles     raster;
            Quantiles     convert;
            Quantiles     present;
            Quantiles     flush;
            Quantiles     input_to_present;

            // Frames whose four phases summed past the pacing budget.
            std::size_t   over_budget    = 0;

            std::uint64_t damaged_pixels = 0;
            std::uint64_t events_drained = 0;
    };

    // Thread-safe: the reactor thread records while a reporter reads. The
    // lock is uncontended at 60 Hz and costs far less than the clock reads
    // that produced the sample.
    void
    record_frame( const FrameSample& sample ) noexcept;

    [[nodiscard]]
    FrameReport
    report() noexcept;

    void
    reset() noexcept;

    // True when `record_frame` has seen a multiple of reportCadence since the
    // last reset — the cue to emit a periodic report.
    [[nodiscard]]
    bool
    due_for_report() noexcept;

    // Writes a report to the log at `level` under the `frame` tag.
    void
    log_report( const FrameReport& summary,
                log::Level         level ) noexcept;

}    // namespace grab::diag
