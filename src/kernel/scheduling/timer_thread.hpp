#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

// The timing authority, which EXECUTES NOTHING.
//
// This is a structural invariant, not a mitigation. screen.capture is
// synchronous; if it ran on the thread that owns deadlines it would slip every
// deadline in the frontier by its full duration — tens of milliseconds, which
// dwarfs every other precision concern in this design. So the component that
// owns deadlines is made structurally incapable of running a command body: it
// takes no callback. It computes when the next step is due, waits, and makes
// its wake_fd() readable. Nothing else.
//
// There is deliberately no member of this class that accepts a callable. The
// invariant is enforced by the absence of the seam, not by a comment asking
// callers not to use one.
//
// wake_fd() is an eventfd the internal thread writes when the earliest armed
// deadline expires, so a reactor or a CLI loop can poll() it alongside
// everything else it already watches. Without it the class would be pull-only
// and would need no thread at all.
//
// Thread safety: arm(), cancel(), drain() and wake_fd() may be called
// concurrently from any thread. stop() and the destructor are teardown and
// must not run concurrently with each other.
//
// ── What it measures about itself ──────────────────────
//
// A timing spine that nobody has measured is a hope, not a spine. The one
// number that decides whether this class is worth anything is WAKE LATENCY:
// how far past its requested deadline a token actually fired. Everything else
// here exists to explain a bad one — a deep queue, a spurious wake, a rearm
// caused by a nearer deadline arriving after the timerfd was already
// programmed.
//
// Two shapes, deliberately kept apart. Durations live in the diag::Instrument
// (`instrument()`), which reports calls/total/min/max/mean per name and is
// what a generic report formatter consumes. Counts live in ScheduleCounters
// (`counters()`), because a queue depth of 3 rendered by a duration formatter
// reads as "3 ns" and is worse than not being reported.
//
// COST. Wake latency and every counter are FREE: run() already reads the clock
// once per loop iteration to decide what expired, and every depth is the size
// of a vector the lock is already held over. Nothing was added to the hot path
// to obtain them, which is why they are unconditional and `--trace` works in
// every build. The per-call costs (arm/cancel/drain) are the exception — they
// need their own clock reads — so they are gated at Verbose and vanish below
// it, exactly like diag::Scope.

#include "kernel/support/step_diag.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace grab::kernel::scheduling
{

    // Instrument slot names. Spelled once, as inline constexpr views, so the
    // Instrument's pointer-equality fast path hits and a reader can find the
    // tally it wants without a content compare.
    namespace timer_tally
    {

        inline constexpr std::string_view wakeLatency = "wake latency";
        inline constexpr std::string_view arm         = "arm";
        inline constexpr std::string_view cancel      = "cancel";
        inline constexpr std::string_view drain       = "drain";

    }    // namespace timer_tally

    // The count-shaped half of the scheduler's self-report. Sums are carried
    // beside the maxima so a mean is derivable without a second pass, and
    // because a mean the class computed could not be re-derived by a caller
    // that merges two runs.
    struct ScheduleCounters
    {
            // arm() calls that actually recorded an entry. An arm() after
            // stop() hands out a token and records nothing, and is not counted
            // here — it never becomes a deadline.
            std::uint64_t arms{};
            std::uint64_t cancels{};

            // Tokens whose deadline passed and that were moved into due_.
            // This is the denominator of the wake-latency tally.
            std::uint64_t fires{};

            std::uint64_t drains{};

            // drain() calls that came back empty: a wake that cost a syscall,
            // a lock and a round trip and delivered nothing. Pure overhead,
            // and the first thing to look at when a run burns CPU idling.
            std::uint64_t spuriousDrains{};

            // An arm() whose deadline was NEARER than the one the timerfd was
            // already programmed for, so the wait had to be torn down and
            // reprogrammed. Cheap individually; a high count against a low
            // fire count means the caller is arming in the wrong order.
            std::uint64_t nearerRearms{};

            // Queue depth. `armed` is measured at each arm (after the push),
            // `due` at each drain (before the swap).
            std::size_t   deepestArmed{};
            std::size_t   deepestDue{};
            std::uint64_t armedDepthTotal{};
            std::uint64_t dueDepthTotal{};
    };

    class TimerThread final
    {
        public:

            using Token = std::uint64_t;

            TimerThread();
            ~TimerThread();

            TimerThread( const TimerThread& ) = delete;
            TimerThread&
            operator=( const TimerThread& )       = delete;
            TimerThread( TimerThread&& ) noexcept = delete;
            TimerThread&
            operator=( TimerThread&& ) noexcept = delete;

            // steady_clock's epoch is CLOCK_MONOTONIC's on glibc/libstdc++,
            // which is what makes an absolute deadline usable directly with
            // TFD_TIMER_ABSTIME. This is a Linux/glibc assumption.
            //
            // Tokens are unique for the lifetime of the object and are never
            // reused. After stop(), or if the timer thread failed to start,
            // arm() still returns a fresh token but records nothing: that token
            // will never be returned by drain().
            [[nodiscard]]
            Token
            arm( std::chrono::steady_clock::time_point deadline );

            // After cancel() returns, the token is not returned by any
            // subsequent drain(), whether or not its deadline had already
            // passed. Cancelling an unknown or already-drained token is a
            // no-op.
            void
            cancel( Token token );

            // Readable when at least one armed deadline has expired. -1 if the
            // object failed to acquire its descriptors, which is the only way
            // construction can fail without a Result to report it through.
            [[nodiscard]]
            int
            wake_fd() const noexcept;

            // The tokens whose deadlines have passed, removed from the timer
            // set. Empty is the normal answer. Clears the readability of
            // wake_fd() atomically with taking the tokens, so a wake is never
            // lost to a concurrent expiry.
            [[nodiscard]]
            std::vector<Token>
            drain();

            // Ends the internal thread. Already-expired tokens stay drainable;
            // still-armed ones never expire. Idempotent. Emits the run's
            // scheduling summary at nominal, once.
            void
            stop() noexcept;

            // ── Introspection ──────────────────────────────────
            //
            // Both of these SNAPSHOT the internal state under the one mutex
            // this class owns and hand back a copy. There is no second lock to
            // order against, and no reference into state the timer thread is
            // still writing — which is the whole reason the reference below is
            // to a member of this object rather than to the Impl's.
            //
            // Call them from one thread at a time: the snapshot they fill is a
            // single buffer, so two concurrent readers would race on it even
            // though neither races the timer thread. In practice the reader is
            // the run's owner, after the run.

            [[nodiscard]]
            const grab::diag::Instrument&
            instrument() const noexcept;

            [[nodiscard]]
            ScheduleCounters
            counters() const noexcept;

            // The largest gap between a token's requested deadline and the
            // instant the timer thread noticed it had passed. Never negative:
            // a token is only collected once `deadline <= now`. Zero when
            // nothing has fired.
            [[nodiscard]]
            std::chrono::nanoseconds
            worst_wake_latency() const noexcept;

        private:

            class Impl;

            std::unique_ptr<Impl>          impl_;

            // Where instrument() puts what it copied out from under the lock.
            // Mutable because reading a snapshot is a query, and a query that
            // forced the caller to hold a non-const handle would push the lock
            // back out into every caller.
            mutable grab::diag::Instrument snapshot_{};
    };

}    // namespace grab::kernel::scheduling
