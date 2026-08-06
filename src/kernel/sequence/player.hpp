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
// keeps the whole scheduler off a display. The only clock read anywhere in
// this class is the run id minted in the constructor, which is identity, not
// scheduling.
//
// THE RUNNER IS A SEAM, NOT A DEPENDENCY. The Player drives commands through
// the abstract CommandRunner below rather than through execute.hpp, for the
// same reason ExecContext takes a seat instead of a grab::Input*: a scheduler
// that can only be exercised against a live X display is a scheduler nobody
// can test. The production runner adapts enter/tick/exit from execute.hpp; a
// test substitutes a fake and fabricates both the clock and the outcomes.

#include "grab/command.hpp"
#include "grab/ids.hpp"
#include "grab/result.hpp"
#include "grab/sequence_types.hpp"
#include "grab/trace.hpp"
#include "kernel/sequence/sequence.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace grab::kernel::sequence
{

    // How the Player runs one step. Mirrors execute.hpp's enter/tick/exit
    // triple without depending on it, so the scheduler and the command bodies
    // can be written, tested and broken independently.
    //
    // exit() ALWAYS runs on an entered step, exactly once, and is what
    // releases a held button or key. The caller owns what it presses: a button
    // left down survives the process and reaches the next application.
    class CommandRunner
    {
        public:

            CommandRunner()                       = default;
            virtual ~CommandRunner()              = default;
            CommandRunner( const CommandRunner& ) = delete;
            CommandRunner&
            operator=( const CommandRunner& ) = delete;
            CommandRunner( CommandRunner&& )  = delete;
            CommandRunner&
            operator=( CommandRunner&& ) = delete;

            // Begin the step. Running means "tick me"; an Instant command
            // answers Success here and never needs a tick.
            [[nodiscard]]
            virtual grab::sequence::Status
            enter( const grab::sequence::Step&           step,
                   std::chrono::steady_clock::time_point now ) = 0;

            [[nodiscard]]
            virtual grab::sequence::Status
            tick( const grab::sequence::Step&           step,
                  std::chrono::steady_clock::time_point now ) = 0;

            // Runs once per entered step, on every path including interrupt,
            // abort, skip and goto. Reports what it released so the run can
            // record a NeutralizationOutcome.
            virtual grab::NeutralizationOutcome
            exit( const grab::sequence::Step&           step,
                  std::chrono::steady_clock::time_point now ) = 0;

            // A Blocking body runs on a worker (design §4.4), so the unwind
            // path JOINS it before exit(). It costs the caller the rest of that
            // step, and that is the correct trade: abandoning the worker would
            // race exit() against a capture still writing its buffer. Called
            // only on the unwind path, and always before exit().
            virtual void
            join( const grab::sequence::Step& step )
            {
                ( void )step;
            }

            // Why the last enter()/tick() answered Failure. Read rather than
            // guessed, because ErrorCode::PossiblyCommitted is never retried
            // regardless of the descriptor's RetryClass.
            [[nodiscard]]
            virtual grab::ErrorCode
            last_error( const grab::sequence::Step& step ) const
            {
                ( void )step;
                return grab::ErrorCode::ProviderFailed;
            }

            // When this running step next wants a tick. nullopt means "every
            // pump"; a value is what a timer thread would arm. Purely an
            // optimisation of the wake-up schedule — the answer must not
            // change with it.
            [[nodiscard]]
            virtual std::optional<std::chrono::steady_clock::time_point>
            next_tick( const grab::sequence::Step& step ) const
            {
                ( void )step;
                return std::nullopt;
            }

            // The step's declared duration, per design §4.5. nullopt means
            // UNKNOWN, SO MEASURE IT — never zero. The default answers only
            // for time.wait, the one op whose duration is mandatory in the
            // document.
            [[nodiscard]]
            virtual std::optional<std::chrono::nanoseconds>
            declared_duration( const grab::sequence::Step& step ) const;
    };

    // The gap between a step becoming ready and its enter(), per design §4.10.
    //
    // THE MODE IS THE SOLE AUTHORITY. extra_grace is read only under Precise,
    // because otherwise "strict allows no gaps" would be a lie any single step
    // could tell. Grace applies only to steps with at least one predecessor —
    // roots start immediately, which is what "between" means.
    //
    // It is a SCHEDULING PROPERTY, never an injected node: injecting waits
    // would change the step count, so the same document would produce
    // different StepIds in different modes.
    [[nodiscard]]
    std::chrono::nanoseconds
    grace_before( const grab::sequence::Step&   step,
                  grab::sequence::PacingOptions pacing ) noexcept;

    // The descriptor table's RetryClass column for a step's command. The
    // runner reads this column; it does not get a retry policy of its own.
    [[nodiscard]]
    grab::RetryClass
    retry_class_of_step( const grab::sequence::Step& step ) noexcept;

    // ErrorCode::PossiblyCommitted is NEVER retried regardless of class: a
    // failed drag has a button down, and re-sending it would press a second
    // time while pretending the first did not happen.
    [[nodiscard]]
    bool
                                   may_retry( const grab::sequence::Step& step,
                                              grab::ErrorCode             code ) noexcept;

    // A step may be re-entered at most this many times before its ErrorPolicy
    // applies. The count is the Player's business; the class is not.
    inline constexpr std::uint32_t maxRetriesPerStep = 1U;

    class Player final
    {
        public:

            Player( const Sequence& program,
                    CommandRunner&  runner );

            // Admit the roots and start pumping. Also resumes from Paused.
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
            // unrelated parallel branches. Steps that are neither ancestor nor
            // descendant are left Pending and are not waited on — a run
            // reaches Done when the FRONTIER EMPTIES, not when every step is
            // terminal. Forward-only: backward motion means re-running
            // effects, which needs its own design.
            [[nodiscard]]
            grab::Result<void>
            goto_step( grab::sequence::StepId target );

            // A label that resolves to nothing is an error, never a silent
            // no-op.
            [[nodiscard]]
            grab::Result<void>
            goto_label( std::string_view label );

            // Advance the run to `now`. Fails only when a step failed under
            // ErrorPolicy::Abort, which is also what unwinds the run — abort
            // and interrupt() share one path.
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

            // Entered steps, oldest first. The unwind walks it backwards.
            [[nodiscard]]
            std::span<const grab::sequence::StepId>
            entry_order() const noexcept;

            // The earliest time at which pumping could change anything: the
            // minimum over ready gaps and requested tick deadlines. This is
            // what a timer thread arms, and what lets a test cross five
            // seconds of virtual time in a handful of pumps. nullopt when the
            // run cannot advance on its own.
            [[nodiscard]]
            std::optional<std::chrono::steady_clock::time_point>
            next_deadline() const;

            [[nodiscard]]
            std::optional<std::chrono::steady_clock::time_point>
            ready_at( grab::sequence::StepId id ) const;

            [[nodiscard]]
            std::optional<std::chrono::steady_clock::time_point>
            entered_at( grab::sequence::StepId id ) const;

            [[nodiscard]]
            std::optional<std::chrono::steady_clock::time_point>
            finished_at( grab::sequence::StepId id ) const;

            // Measured wall span of the run in the caller's fabricated clock:
            // last completion minus first entry. Zero before anything entered.
            [[nodiscard]]
            std::chrono::nanoseconds
            elapsed() const noexcept;

            [[nodiscard]]
            grab::sequence::StepTiming
            timing_of( grab::sequence::StepId id ) const;

            // How far past its declared duration the step ran, zero when it
            // declared none or stayed inside it. The diagnostic that the
            // automated app is slow or hung.
            [[nodiscard]]
            std::chrono::nanoseconds
            overrun_of( grab::sequence::StepId id ) const;

            // What the unwind released. NotAttempted until a step is exited
            // before its body completed — interrupt, abort, skip or goto.
            [[nodiscard]]
            grab::NeutralizationOutcome
            neutralization() const noexcept;

            // The failure that aborted the run, or nullptr.
            [[nodiscard]]
            const grab::Error*
            failure() const noexcept;

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

            // Per-step run state, parallel to steps() and subscripted by
            // StepId's index half. Flat vectors rather than a map: the
            // frontier is 2-10 wide and the document is contiguous already.
            struct StepRun
            {
                    std::chrono::steady_clock::time_point   ready_at{};
                    std::chrono::steady_clock::time_point   entered_at{};
                    std::chrono::steady_clock::time_point   finished_at{};
                    std::optional<std::chrono::nanoseconds> declared{};
                    std::chrono::nanoseconds                call_duration{};
                    std::uint32_t                           retries{ 0U };
                    bool                                    entered{ false };
                    bool                                    exited{ false };
            };

            [[nodiscard]]
            std::size_t
            index_of( grab::sequence::StepId id ) const;

            [[nodiscard]]
            bool
            is_terminal( grab::sequence::StepId id ) const;

            void
            drop_from_frontier( grab::sequence::StepId id );

            // exit() once, joining a blocking worker first. Folds the outcome
            // into neutralization_ only when the body had not completed, so a
            // clean run still reports NotAttempted.
            void
            neutralize( grab::sequence::StepId                id,
                        std::chrono::steady_clock::time_point now );

            void
            admit_successors( grab::sequence::StepId                id,
                              std::chrono::steady_clock::time_point now );

            void
            on_status( grab::sequence::StepId                id,
                       grab::sequence::Status                status,
                       std::chrono::steady_clock::time_point now );

            void
            succeed( grab::sequence::StepId                id,
                     std::chrono::steady_clock::time_point now );

            void
            fail_step( grab::sequence::StepId                id,
                       grab::ErrorCode                       code,
                       std::chrono::steady_clock::time_point now );

            void
            unwind( std::chrono::steady_clock::time_point now );

            [[nodiscard]]
            grab::Result<void>
                            jump_to( grab::sequence::StepId target );

            const Sequence* program_{ nullptr };
            CommandRunner*  runner_{ nullptr };
            std::vector<grab::sequence::StepStatus> status_;
            std::vector<StepRun>                    runs_;
            std::vector<grab::sequence::StepId>     frontier_;
            std::vector<grab::sequence::StepId>     entry_order_;
            // Successors in CSR form: successors of step i are
            // successors_[ successor_offsets_[i] .. successor_offsets_[i+1] ).
            std::vector<std::size_t>                successor_offsets_;
            std::vector<grab::sequence::StepId>     successors_;
            // Scratch for the pump's fixpoint loop, so a pass over the
            // frontier is not invalidated by the admissions it causes.
            std::vector<grab::sequence::StepId>     scratch_;
            grab::sequence::PlayState   state_{ grab::sequence::PlayState::Idle };
            grab::NeutralizationOutcome neutralization_{
                grab::NeutralizationOutcome::NotAttempted,
            };
            std::optional<grab::Error>            failure_{};
            std::chrono::steady_clock::time_point last_now_{};
            std::chrono::steady_clock::time_point first_entry_{};
            std::chrono::steady_clock::time_point last_finish_{};
            bool                                  anything_entered_{ false };
            grab::Uuid                            run_id_{};
    };

}    // namespace grab::kernel::sequence
