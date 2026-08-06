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

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace grab::kernel::scheduling
{

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
            // still-armed ones never expire. Idempotent.
            void
            stop() noexcept;

        private:

            class Impl;

            std::unique_ptr<Impl> impl_;
    };

}    // namespace grab::kernel::scheduling
