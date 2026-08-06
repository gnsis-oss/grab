#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

// The mutable half of the document/run split: every piece of run state lives
// here, so reverting a run is dropping the Player.
//
// THE CURSOR IS A FRONTIER. With parallel branches there is no single "current
// step"; the position is the set of steps ready or running.
//
// pump( now ) is CALLER-DRIVEN and reads no clock of its own. The reactor
// pumps it in production; a test pumps it with fabricated timestamps, which is
// what makes a five-second wait assertable in microseconds of real time and
// keeps the whole scheduler off a display.
//
// PHASE 0: declaration only. The player unit implements it.

#include "grab/ids.hpp"
#include "grab/result.hpp"
#include "grab/sequence_types.hpp"
#include "kernel/sequence/sequence.hpp"

#include <chrono>
#include <span>
#include <string_view>
#include <vector>

namespace grab::kernel::sequence
{

    class Player final
    {
        public:

            explicit Player( const Sequence& program );

            // Pump the frontier.
            [[nodiscard]]
            grab::Result<void>
            play();

            // Admit no new steps. RUNNING STEPS RUN TO COMPLETION — pausing
            // mid-drag would strand a held button.
            [[nodiscard]]
            grab::Result<void>
            pause();

            // Stop, then exit() every entered step in reverse entry order,
            // releasing held buttons and keys. Grace does NOT apply on this
            // path: a held button must not wait out a grace period before
            // being released. While a blocking body is in flight this joins
            // the worker first and returns only after — abandoning it would
            // race exit() against a capture still writing its buffer.
            [[nodiscard]]
            grab::Result<void>
            interrupt();

            // Mark the current frontier skipped and advance to its successors.
            // A step that is already Running has been ENTERED, so exit() runs
            // on it: skipping never strands a held button.
            [[nodiscard]]
            grab::Result<void>
            skip();

            // Mark every ANCESTOR of the target that is not already terminal
            // as skipped, and set the frontier to { target }. Defined over
            // ancestors rather than over order(), because order() is one
            // arbitrary linearization and skipping its prefix would skip
            // unrelated parallel branches. Forward-only: backward motion means
            // re-running effects, which needs its own design.
            [[nodiscard]]
            grab::Result<void>
            goto_step( grab::sequence::StepId target );

            [[nodiscard]]
            grab::Result<void>
            goto_label( std::string_view label );

            [[nodiscard]]
            grab::Result<void>
            pump( std::chrono::steady_clock::time_point now );

            [[nodiscard]]
            grab::sequence::PlayState
            state() const noexcept;

            [[nodiscard]]
            grab::sequence::StepStatus
            status_of( grab::sequence::StepId id ) const;

            [[nodiscard]]
            std::span<const grab::sequence::StepId>
            frontier() const noexcept;

            // One per run, so reports from different sessions never collide.
            [[nodiscard]]
            grab::Uuid
            run_id() const noexcept;

            // The document being played. Borrowed, never owned: a Player is
            // the run state and nothing else.
            [[nodiscard]]
            const Sequence*
            program() const noexcept;

        private:

            const Sequence*                         program_{ nullptr };
            std::vector<grab::sequence::StepStatus> status_;
            std::vector<grab::sequence::StepId>     frontier_;
            grab::sequence::PlayState state_{ grab::sequence::PlayState::Idle };
            grab::Uuid                run_id_{};
    };

}    // namespace grab::kernel::sequence
