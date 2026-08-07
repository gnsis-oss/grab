#pragma once

// `grab play` -- load a JSON sequence document, build it, and run it through
// the Player over a seat adapted from grab::Input.
//
//   grab play <sequence.json> [--pacing strict|grace|precise] [--grace-ms N]
//                             [--dry-run] [--report <path.jsonl>] [--trace]
//                             [--trail] [--feedback] + their style flags
//
// VISUAL FEEDBACK IS TWO FLAGS, NOT THREE PROCESSES. Watching a playback used
// to mean `grab trail &`, `grab feedback &`, then `grab play` -- three
// processes started in the right order, each opening its own Session against
// the same display, and each needing to be killed afterwards. `--trail` and
// `--feedback` do it from the one command, over the one session the overlay
// steps already use.
//
// CLI FLAGS OVERRIDE THE DOCUMENT. `pacing` is a document block and also a
// pair of flags, and when they disagree the flags win -- that is what lets one
// document run tight or loose without being edited. The override is applied by
// REBUILDING the Sequence with the effective pacing rather than by carrying a
// second copy of it, because Player reads grace from program()->pacing() and
// would otherwise pace the run by the document while the plan was printed
// under the flag.
//
// EXIT CODES, which the corpus harness is written against:
//
//   0  the document loaded, and either --dry-run printed its plan or the run
//      reached Done
//   1  the document failed to load or validate, or a step failed under
//      ErrorPolicy::Abort, or the seat could not be opened
//   2  the command line itself was wrong -- an unknown flag, a missing value,
//      no document, or more than one
//
// A rejected document is exit 1 and a message on stderr carrying the loader's
// JSON pointer, e.g.
//
//   grab: error: doc.json: /steps/3/after/0: step 'shot': names no step
//
// THE SEAT IS A TEMPLATE PARAMETER for the same reason ExecContext's is: a
// runner that can only be exercised against a live X display is a runner
// nobody can test. SeatRunner below is the production CommandRunner and takes
// any seat satisfying the concepts in execute.hpp, which includes
// grab::testing::RecordingSeat.

#include "frontends/cli/overlay_command.hpp"
#include "grab/command.hpp"
#include "grab/command_descriptor.hpp"
#include "grab/overlay.hpp"
#include "grab/result.hpp"
#include "grab/sequence_types.hpp"
#include "grab/session.hpp"
#include "grab/trace.hpp"
#include "kernel/presentation/trail_animator.hpp"
#include "kernel/scheduling/timer_thread.hpp"
#include "kernel/sequence/execute.hpp"
#include "kernel/sequence/player.hpp"
#include "kernel/sequence/sequence.hpp"
#include "kernel/support/step_diag.hpp"

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace grab::cli
{

    // What the command line asked for. Every override is optional so that
    // "absent" and "explicitly the default" stay distinguishable: --pacing
    // strict on a grace document must force strict, and no flag at all must
    // leave the document alone.
    struct PlayOptions
    {
            std::string                               document{};
            std::string                               report{};
            std::optional<grab::sequence::PacingMode> pacing{};
            std::optional<std::chrono::milliseconds>  grace{};
            bool                                      dry_run{ false };

            // --trace: print the END-OF-RUN timing summary. Deliberately not
            // a log level. `--log-level verbose --log-tags timer` is the
            // running commentary -- one record per arm, per fire, per cancel,
            // interleaved with the run and costing a write(2) each. This is
            // the accounting afterwards, built from tallies that were
            // accumulating anyway. They compose: asking for both gives the
            // commentary and then the summary, and neither is derived from
            // the other.
            bool                                      trace{ false };

            // ── Visual feedback, opt-in ───────────────────────
            //
            // Two booleans, so ONE command plays a document AND shows what it
            // is doing. Both are off by default: a headless corpus run must not
            // start paying for an overlay it will never look at.
            //
            // The style fields below are meaningful only alongside their
            // feature. A style flag without its feature is a COMMAND-LINE ERROR
            // naming the flag that is missing, never a silent no-op -- the
            // failure mode being avoided is `--fade-ms 400` on its own, which
            // would otherwise parse, run, and draw nothing.
            bool                                      trail{ false };
            bool                                      feedback{ false };

            // Spelled out rather than defaulted, because
            // `OverlayTrailOptions{}` is NOT the trail's defaults: its `fade`
            // and `width_px` default to zero, and the standalone verb fills
            // them in at parse time. A zero-width trail draws nothing.
            //
            // BOTH COLOURS DEFAULT TO THE SAME AMBER, which is what makes
            // `grab play --trail` visible out of the box: under playback every
            // sample is XTest-injected, so the trail is drawn ENTIRELY in
            // `injected_color` and a dim or distinct default there would look
            // like a broken feature rather than a deliberate one.
            OverlayTrailOptions                       trail_style{
                .physical_color = grab::overlay::defaultOverlayColor,
                .injected_color = grab::overlay::defaultOverlayColor,
                .fade           = grab::kernel::presentation::defaultTrailFade,
                .width_px       = grab::kernel::presentation::defaultTrailWidthPx,
            };

            // Click ripple and hold bar both on, matching `grab feedback`.
            // `CursorFeedbackConfig{}` would leave both nullopt, which is the
            // configuration that presents nothing.
            CursorFeedbackConfig feedback_style{
                .click      = grab::RippleStyle{},
                .hold       = grab::ProgressStyle{},
                .thresholds = grab::GestureThresholds{},
            };
    };

    // ── What --trace reports ──────────────────────────────
    //
    // Four instruments, kept apart because they answer four different
    // questions and merging them would lose which is which:
    //
    //   load       parsing and building the document, once, before any clock
    //              that matters starts
    //   run        per-command wall time, which is where a sequence actually
    //              spends itself
    //   pump       the Player's own phases, when it exposes them
    //   scheduling the timer thread's accuracy -- the number that says whether
    //              the run was slow because the work was slow or because the
    //              spine was
    //
    // Every one of them is a diag::Instrument, so the same formatter prints
    // all four and a new phase anywhere shows up without touching this file.
    struct RunTrace
    {
            std::string                                sequence{};
            std::size_t                                steps{};

            // Measured span of the run, from the Player's own clock.
            std::chrono::nanoseconds                   elapsed{};

            // What the document said it would take, recomputed under the
            // EFFECTIVE pacing, and how many steps declared nothing. A plan of
            // 7 s over 199 steps with 187 unestimated is a lower bound and
            // must read as one.
            std::chrono::nanoseconds                   planned{};
            std::size_t                                unestimated{};

            // Whole-document load: read, parse, validate and rebuild under the
            // effective pacing. Timed unconditionally -- it happens once, and
            // two clock reads for a number a human asked for is not a hot
            // path.
            std::chrono::nanoseconds                   load{};

            grab::diag::Instrument                     load_phases{};
            grab::diag::Instrument                     run{};
            grab::diag::Instrument                     pump{};
            grab::diag::Instrument                     scheduling{};
            grab::kernel::scheduling::ScheduleCounters schedule{};

            // False for --dry-run: nothing was played, so a run section would
            // be a table of zeroes pretending to be measurements.
            bool                                       ran{ false };
    };

    // The report, as text rather than to a stream, so its shape is assertable
    // without capturing stdout. Ends in a newline. Sections are sorted by
    // total time descending, so the expensive thing is the first line a human
    // reads.
    [[nodiscard]]
    std::string
    trace_report( const RunTrace& trace );

    // Per-command tallies, derived from the Player's public timing. This is
    // deliberately NOT dependent on the Player exposing an instrument: every
    // number here comes from timing_of(), which is the same source the JSONL
    // has always used, so the pretty report and the machine-readable one
    // cannot disagree.
    void
    collect_run_tallies( const grab::kernel::sequence::Sequence& program,
                         const grab::kernel::sequence::Player&   player,
                         RunTrace&                               into );

    // The Player's own pump-phase instrument, when it has one. A template so
    // the absent case is a discarded branch rather than a compile error --
    // the accessor is landing in a unit written beside this one, and a report
    // that cannot be built until that lands is a report nobody gets today.
    template<typename PlayerT>
    void
    collect_pump_tallies( const PlayerT& player,
                          RunTrace&      into )
    {
        if constexpr( requires {
                          {
                              player.instrument()
                          } -> std::convertible_to<const grab::diag::Instrument&>;
                      } )
        {
            into.pump = player.instrument();
        }
        else
        {
            static_cast<void>( player );
            static_cast<void>( into );
        }
    }

    [[nodiscard]]
    grab::Result<PlayOptions>
    parse_play_options( std::span<const std::string_view> args );

    // The document's pacing with the CLI overrides applied. The mode and the
    // grace override independently: --grace-ms alone changes the interval
    // without changing which mode reads it, and under Strict the mode remains
    // the sole authority and ignores it.
    [[nodiscard]]
    grab::sequence::PacingOptions
    effective_pacing( grab::sequence::PacingOptions document,
                      const PlayOptions&            options ) noexcept;

    // The same document under different pacing. Ids are positional, so
    // rebuilding preserves every StepId: the same document produces the same
    // identities in all three modes, which is what the round-trip test
    // depends on.
    [[nodiscard]]
    grab::Result<grab::kernel::sequence::Sequence>
    with_pacing( const grab::kernel::sequence::Sequence& program,
                 grab::sequence::PacingOptions           pacing );

    // One command as a one-step document. This is what routes `grab click`,
    // `grab type` and `grab drag` through the command layer.
    [[nodiscard]]
    grab::Result<grab::kernel::sequence::Sequence>
    single_step_sequence( grab::sequence::Command command );

    // What --dry-run prints, as text rather than to a stream, so the format is
    // assertable without capturing stdout. Ends in a newline.
    //
    //   sequence: login-flow
    //   pacing: grace grace_ms=80
    //   steps: 5
    //   order: 0 1 2 3 4
    //   step 0 'move' input.move after=[]
    //   step 1 'wait' time.wait after=[0]
    //   plan: >= 490 ms, 4 steps unestimated
    //
    // The plan is recomputed under `pacing`, NOT read from the document:
    // printing the document's figure while running under --pacing grace would
    // be silently wrong.
    [[nodiscard]]
    std::string
    dry_run_report( const grab::kernel::sequence::Sequence& program,
                    grab::sequence::PacingOptions           pacing );

    // One JSON object per step, in topological order, carrying what the run
    // recorded for it. Separate from writing so a test can assert the records
    // without touching the filesystem.
    //
    // THE PER-STEP RECORD SHAPE IS ADDITIVE AND STAYS THAT WAY: the corpus
    // harness is written against it, so `start_ns`, `wait_ns` and `end_ns`
    // joined the existing keys rather than replacing any. `wait_ns` is the
    // span a step sat Ready before it was entered, which is the pacing and
    // scheduling cost attributable to that step; `start_ns` and `end_ns` are
    // relative to the first entry of the run, so a consumer can lay the run
    // out on a timeline without knowing this process's steady-clock origin.
    //
    // `trace` adds ONE extra line at the end, tagged `"kind":"trace"`, holding
    // the run-level accounting. It is written only when --trace was asked for,
    // so a consumer counting one line per step keeps counting one line per
    // step unless it opted in.
    [[nodiscard]]
    std::vector<std::string>
    report_records( const grab::kernel::sequence::Sequence& program,
                    const grab::kernel::sequence::Player&   player,
                    const RunTrace*                         trace = nullptr );

    [[nodiscard]]
    grab::Result<void>
    write_report( const std::string&              path,
                  const std::vector<std::string>& records );

    // Pump the run to completion. Caller-driven everywhere else in this
    // design; here the caller is the process, so this is where a real clock is
    // finally read. It does not sleep -- the wait is a TimerThread deadline
    // and a poll() on its eventfd, which is the sanctioned mechanism and the
    // reason src/ carries no raw sleeps.
    //
    // Fails only when a step failed under ErrorPolicy::Abort. The run is
    // always left in a terminal state: a loop that ends any other way
    // interrupts the player first, so nothing stays entered.
    //
    // `trace`, when given, receives the timer thread's accuracy. It is
    // harvested HERE and nowhere else because the TimerThread is a local of
    // this function: it is created on the first wait and destroyed on return,
    // so a caller has no other moment at which to ask it anything.
    //
    // `on_pump` runs once per pump, on this loop's own thread. `--trail` uses
    // it to turn the observation queue into trail segments WHILE the run
    // proceeds: this loop is the only thread awake for the whole run, and a
    // trail assembled afterwards is a trail nobody saw. It is called on the
    // last iteration too, so the tail of a run is not dropped.
    using PumpHook = std::function<void()>;

    [[nodiscard]]
    grab::Result<void>
    drive( grab::kernel::sequence::Player& player,
           RunTrace*                       trace   = nullptr,
           const PumpHook&                 on_pump = PumpHook{} );

    // Build the player, drive it, write the report, and answer the process
    // exit code. Takes the runner rather than making one so the
    // failure-to-exit-code mapping is testable without a display.
    [[nodiscard]]
    int
    play_program( const grab::kernel::sequence::Sequence& program,
                  grab::kernel::sequence::CommandRunner&  runner,
                  const PlayOptions&                      options,
                  RunTrace*                               trace   = nullptr,
                  const PumpHook&                         on_pump = PumpHook{} );

    [[nodiscard]]
    int
    run_play_command( std::span<const std::string_view> args );

    // Runs one command as a one-step document over a seat adapted from
    // grab::Input. `grab click`, `grab type` and `grab drag` go through here,
    // which is the parity forcing function for the layered design: without a
    // real consumer the command layer drifts behind grab::Input.
    [[nodiscard]]
    grab::Result<void>
    play_single_command( grab::sequence::Command command,
                         const char*             display,
                         std::string_view        layout );

    // The production CommandRunner.
    //
    // THE CONTRACT, and what goes wrong when it is broken:
    //
    //  - exit() runs exactly once per entered step, on every path. The Player
    //    guarantees the call; this class guarantees the once.
    //  - exit() must know whether the run is UNWINDING, because an explicit
    //    hold -- the button input.press leaves down, the key input.key_down
    //    leaves down so a chord can be spelled -- is released only when no
    //    later step will release it. The Player calls the same exit() on both
    //    paths, so the signal is derived here: a step whose last status was
    //    Success completed and is exited normally; anything else (Running when
    //    the frontier was torn down, or Failure) is an unwind, and
    //    interrupt() sets state.interrupted before exit().
    //  - Player::unwind skips steps it has already exited, and a SUCCESSFUL
    //    input.press was already exited by succeed(). Its hold therefore
    //    survives the unwind, so TWO mechanisms lift it and neither is
    //    redundant:
    //
    //      1. release_holds(), which the Player's unwind calls for exactly
    //         those already-exited steps. It runs while the document is being
    //         torn down, which is as early as the hold can honestly be
    //         released.
    //      2. release_outstanding(), which the CLI calls on every exit path.
    //         It is the backstop for the paths no unwind reaches at all — a
    //         run that reaches Done with a press or an overlay.grab still
    //         outstanding, because the document simply never released it.
    //
    //    Without (1) an aborted run holds the pointer for the whole of its
    //    report-writing; without (2) a COMPLETED run holds it forever. A
    //    stranded button reaches the next application; a stranded pointer
    //    capture freezes the whole desktop, and recovery needs another X
    //    client or a VT switch. That asymmetry is why the capture is tracked
    //    beside the buttons and keys rather than trusted to the executor.
    template<typename SeatT>
    class SeatRunner final : public grab::kernel::sequence::CommandRunner
    {
        public:

            using TimePoint = std::chrono::steady_clock::time_point;

            explicit SeatRunner( SeatT& seat ) noexcept :
                seat_( &seat )
            {
            }

            [[nodiscard]]
            grab::sequence::Status
            enter( const grab::sequence::Step& step,
                   TimePoint                   now ) override
            {
                auto& entry = entry_for( step.id );
                // A retry re-enters the same step, so the scratch is reset
                // rather than reused: leaving `interrupted` set from the
                // previous attempt would make the next exit() release a hold
                // the document still owns.
                entry.state        = grab::kernel::sequence::CommandState{};
                entry.exited       = false;

                auto       context = make_context( now );
                const auto status =
                    grab::kernel::sequence::enter( step.command, entry.state, context );
                entry.last_status = status;
                if( status == grab::sequence::Status::Failure )
                {
                    entry.error = classify( step, entry );
                }
                if( status == grab::sequence::Status::Success )
                {
                    remember_hold( step.command );
                }
                // The capture is remembered WHATEVER the round trip reported.
                // overlay.grab sets its flag before the seat call because past
                // that point the server may have handed this process the
                // pointer however the call answers, and a capture that MIGHT
                // be held has to be released like one that is.
                if( entry.state.overlay_grab_held )
                {
                    capture_held_ = true;
                }
                return status;
            }

            [[nodiscard]]
            grab::sequence::Status
            tick( const grab::sequence::Step& step,
                  TimePoint                   now ) override
            {
                auto* const entry = find( step.id );
                if( entry == nullptr )
                {
                    return grab::sequence::Status::Failure;
                }

                auto       context = make_context( now );
                const auto status  = grab::kernel::sequence::tick( step.command,
                                                                   entry->state,
                                                                   context,
                                                                   now );
                entry->last_status = status;
                if( status == grab::sequence::Status::Failure )
                {
                    entry->error = classify( step, *entry );
                }
                if( status == grab::sequence::Status::Success )
                {
                    remember_hold( step.command );
                }
                return status;
            }

            grab::NeutralizationOutcome
            exit( const grab::sequence::Step& step,
                  TimePoint                   now ) override
            {
                auto* const entry = find( step.id );
                if( entry == nullptr || !entry->state.entered || entry->exited )
                {
                    return grab::NeutralizationOutcome::NotAttempted;
                }
                entry->exited = true;

                // The pointer capture is OR'd into both halves of this
                // question on purpose. Leave it out of `held_before` and a
                // capture that was really released reports NothingHeld, which
                // is a false report about the one hold whose escape freezes
                // the desktop; leave it out of the re-check and a capture the
                // document still owns reports Released, which is a false
                // report in the other direction.
                const bool held_before = entry->state.held ||
                                         entry->state.document_hold ||
                                         entry->state.overlay_grab_held;
                const bool unwinding =
                    entry->last_status != grab::sequence::Status::Success;

                auto context = make_context( now );
                if( unwinding )
                {
                    grab::kernel::sequence::interrupt( step.command,
                                                       entry->state,
                                                       context );
                }
                else
                {
                    grab::kernel::sequence::exit( step.command, entry->state, context );
                }

                if( !held_before )
                {
                    return grab::NeutralizationOutcome::NothingHeld;
                }
                if( entry->state.held ||
                    entry->state.document_hold ||
                    entry->state.overlay_grab_held )
                {
                    // Still down on purpose: the document owns it and a later
                    // step is supposed to lift it. release_outstanding() is
                    // the backstop if that step never runs.
                    return grab::NeutralizationOutcome::NotAttempted;
                }
                forget_hold( step.command );
                return grab::NeutralizationOutcome::Released;
            }

            void
            join( const grab::sequence::Step& step ) override
            {
                // Nothing here runs on a worker: the capture seat below does
                // its work inside begin_capture, so there is no thread to
                // join. The override exists to say so rather than to inherit
                // silence.
                ( void )step;
            }

            [[nodiscard]]
            grab::ErrorCode
            last_error( const grab::sequence::Step& step ) const override
            {
                const auto* const entry = find( step.id );
                return entry == nullptr ? grab::ErrorCode::ProviderFailed : entry->error;
            }

            [[nodiscard]]
            std::optional<TimePoint>
            next_tick( const grab::sequence::Step& step ) const override
            {
                const auto* const entry = find( step.id );
                if( entry == nullptr || !entry->state.entered )
                {
                    return std::nullopt;
                }
                return std::visit(
                    [entry]( const auto& payload ) -> std::optional<TimePoint>
                    {
                        return due_at( payload, entry->state );
                    },
                    step.command
                );
            }

            // Mechanism 1 of design §3.2: the Player's unwind reaches back
            // into a step that already succeeded and lifts what the document
            // deliberately left down, because the later step that was supposed
            // to lift it is never going to run.
            //
            // Only the EXPLICIT holds are touched. An implicit one — the
            // button a drag presses for its own walk — was already released by
            // the body, and pressing it up a second time is a second event the
            // application sees.
            //
            // TWO CONDITIONS, AND BOTH ARE NECESSARY. `state.document_hold`
            // says this step TOOK an explicit hold; it never says the hold is
            // still down, because nothing on the success path clears it —
            // exit_command returns early when the run is not unwinding, which
            // is what makes a chord and a press/release pair expressible at
            // all. So a completed `input.press` → `input.release` pair would
            // be released a SECOND time here on the strength of that flag
            // alone. The runner's own buttons_/keys_/capture_held_ is the half
            // that knows whether anything is still outstanding, because the
            // matching release retires it there. Ask both.
            grab::NeutralizationOutcome
            release_holds( const grab::sequence::Step& step ) override
            {
                auto* const entry = find( step.id );
                if( entry == nullptr || !entry->state.entered )
                {
                    return grab::NeutralizationOutcome::NotAttempted;
                }
                const bool button_or_key =
                    entry->state.document_hold && still_outstanding( step.command );
                const bool capture = entry->state.overlay_grab_held && capture_held_;
                if( !button_or_key && !capture )
                {
                    return grab::NeutralizationOutcome::NotAttempted;
                }

                bool released = true;
                if( button_or_key )
                {
                    // Cleared before the attempt, like exit()'s own releases: a
                    // failing seat must not turn this into an unbounded retry.
                    entry->state.document_hold = false;
                    released = lift_document_hold( step.command ) && released;
                }
                if( capture )
                {
                    entry->state.overlay_grab_held = false;
                    released                       = lift_capture() && released;
                }
                // release_outstanding() must not lift it a second time.
                forget_hold( step.command );

                // Nominal, not verbose: a failed release here is the single
                // worst thing this layer can report, and the question "did the
                // unwind strand anything" is asked after the fact.
                log::nominal(
                    [&step, capture, released]( auto& event )
                    {
                        event.tag( log::tags::player )
                            .value( "reaped",
                                    grab::command_name(
                                        grab::sequence::kind_of( step.command )
                                    ) )
                            .value( "capture", capture )
                            .value( "ok", released );
                    }
                );
                return released ? grab::NeutralizationOutcome::Released
                                : grab::NeutralizationOutcome::Failed;
            }

            // Mechanism 2: lift anything the document left down. The process
            // is the last step: after it exits nothing can release a held
            // button, key or pointer capture, so an outstanding hold here is
            // by definition one no later step will lift.
            grab::NeutralizationOutcome
            release_outstanding()
            {
                if( buttons_.empty() && keys_.empty() && !capture_held_ )
                {
                    return grab::NeutralizationOutcome::NothingHeld;
                }

                bool released = true;
                if constexpr( grab::kernel::sequence::PointerSeat<SeatT> )
                {
                    for( const auto code : buttons_ )
                    {
                        released = seat_->button( code, false ).has_value() && released;
                    }
                }
                if constexpr( grab::kernel::sequence::KeyboardSeat<SeatT> )
                {
                    for( const auto& name : keys_ )
                    {
                        released =
                            seat_->key_by_name( name, false ).has_value() && released;
                    }
                }
                if constexpr( grab::kernel::sequence::PointerSeat<SeatT> )
                {
                    released = seat_->flush().has_value() && released;
                }
                if( capture_held_ )
                {
                    released = lift_capture() && released;
                }
                buttons_.clear();
                keys_.clear();
                return released ? grab::NeutralizationOutcome::Released
                                : grab::NeutralizationOutcome::Failed;
            }

            [[nodiscard]]
            std::size_t
            outstanding_holds() const noexcept
            {
                return buttons_.size() +
                       keys_.size() +
                       static_cast<std::size_t>( capture_held_ ? 1U : 0U );
            }

            // Whether the pointer is still captured as far as this runner
            // knows. Separate from outstanding_holds() because the two failures
            // are an order of magnitude apart: a stranded button reaches the
            // next application, a stranded capture freezes the desktop.
            [[nodiscard]]
            bool
            capture_outstanding() const noexcept
            {
                return capture_held_;
            }

        private:

            // Per-step scratch. Flat and subscripted by the StepId's index
            // half, because a document is contiguous already and the frontier
            // is a handful of steps wide.
            struct Entry
            {
                    grab::kernel::sequence::CommandState state{};
                    grab::sequence::Status               last_status{
                        grab::sequence::Status::Running
                    };
                    grab::ErrorCode error{ grab::ErrorCode::ProviderFailed };
                    bool            used{ false };
                    bool            exited{ false };
            };

            [[nodiscard]]
            grab::kernel::sequence::ExecContext<SeatT>
            make_context( TimePoint now ) const
            {
                return grab::kernel::sequence::ExecContext<SeatT>{
                    .seat   = seat_,
                    .timers = nullptr,
                    .now    = now,
                };
            }

            [[nodiscard]]
            Entry&
            entry_for( grab::sequence::StepId id )
            {
                const auto index = static_cast<std::size_t>( id.index() );
                if( index >= entries_.size() )
                {
                    entries_.resize( index + 1U );
                }
                entries_[index].used = true;
                return entries_[index];
            }

            [[nodiscard]]
            Entry*
            find( grab::sequence::StepId id )
            {
                const auto index = static_cast<std::size_t>( id.index() );
                if( index >= entries_.size() || !entries_[index].used )
                {
                    return nullptr;
                }
                return &entries_[index];
            }

            [[nodiscard]]
            const Entry*
            find( grab::sequence::StepId id ) const
            {
                const auto index = static_cast<std::size_t>( id.index() );
                if( index >= entries_.size() || !entries_[index].used )
                {
                    return nullptr;
                }
                return &entries_[index];
            }

            // Which capability a kind needs, answered against THIS seat. A
            // seat that cannot do what the step asks is a configuration fault,
            // not a transient one, and saying so keeps a missing capability
            // out of the retry path.
            [[nodiscard]]
            static constexpr bool
            capability_available( grab::CommandKind kind ) noexcept
            {
                constexpr bool pointer  = grab::kernel::sequence::PointerSeat<SeatT>;
                constexpr bool keyboard = grab::kernel::sequence::KeyboardSeat<SeatT>;
                constexpr bool text     = grab::kernel::sequence::TextSeat<SeatT>;
                constexpr bool capture  = grab::kernel::sequence::CapturingSeat<SeatT>;
                constexpr bool overlay  = grab::kernel::sequence::OverlaySeat<SeatT>;
                switch( kind )
                {
                    case grab::CommandKind::Warp :
                    case grab::CommandKind::Click :
                    case grab::CommandKind::ClickAt :
                    case grab::CommandKind::Press :
                    case grab::CommandKind::Release :
                    case grab::CommandKind::Scroll :
                    case grab::CommandKind::Move :
                    case grab::CommandKind::Follow :
                    case grab::CommandKind::Drag :
                        return pointer;
                    case grab::CommandKind::Type :
                        return text;
                    case grab::CommandKind::Key :
                    case grab::CommandKind::KeyDown :
                    case grab::CommandKind::KeyUp :
                        return keyboard;
                    case grab::CommandKind::Capture :
                        return capture;
                    case grab::CommandKind::OverlayAdd :
                    case grab::CommandKind::OverlayUpdate :
                    case grab::CommandKind::OverlayRemove :
                    case grab::CommandKind::OverlayClear :
                    case grab::CommandKind::OverlayGrab :
                    case grab::CommandKind::OverlayRelease :
                    case grab::CommandKind::OverlayAttach :
                    case grab::CommandKind::OverlayDetach :
                        return overlay;
                    case grab::CommandKind::Wait :
                        return true;
                    default :
                        return false;
                }
            }

            // The executor answers Status and logs the reason, so the code is
            // derived rather than read. PossiblyCommitted first, because a
            // step that already has something down must never be retried
            // whatever the descriptor's RetryClass says.
            [[nodiscard]]
            static grab::ErrorCode
            classify( const grab::sequence::Step& step,
                      const Entry&                entry ) noexcept
            {
                if( !capability_available( grab::sequence::kind_of( step.command ) ) )
                {
                    return grab::ErrorCode::CapabilityUnavailable;
                }
                if( entry.state.held || entry.state.document_hold )
                {
                    return grab::ErrorCode::PossiblyCommitted;
                }
                return grab::ErrorCode::ProviderFailed;
            }

            void
            remember_hold( const grab::sequence::Command& command )
            {
                if( const auto* const press =
                        std::get_if<grab::sequence::PressCommand>( &command );
                    press != nullptr )
                {
                    if( std::ranges::find( buttons_, press->button ) == buttons_.end() )
                    {
                        buttons_.push_back( press->button );
                    }
                    return;
                }
                if( const auto* const release =
                        std::get_if<grab::sequence::ReleaseCommand>( &command );
                    release != nullptr )
                {
                    std::erase( buttons_, release->button );
                    return;
                }
                if( const auto* const down =
                        std::get_if<grab::sequence::KeyDownCommand>( &command );
                    down != nullptr )
                {
                    if( std::ranges::find( keys_, down->key ) == keys_.end() )
                    {
                        keys_.push_back( down->key );
                    }
                    return;
                }
                if( const auto* const up =
                        std::get_if<grab::sequence::KeyUpCommand>( &command );
                    up != nullptr )
                {
                    std::erase( keys_, up->key );
                    return;
                }
                // A SUCCEEDING overlay.release is the one thing that clears the
                // capture from the outside. Its counterpart is not here: the
                // grab is remembered in enter() whatever it reported, because
                // a failed grab may still hold the pointer. The capture is one
                // flag rather than a list — the server grants the pointer once,
                // and a second overlay.grab over a live one is the same
                // capture, not a second.
                if( std::holds_alternative<grab::sequence::OverlayReleaseCommand>(
                        command
                    ) )
                {
                    capture_held_ = false;
                }
            }

            // exit() already lifted it, so the backstop must not lift it
            // twice: a second release is a second event the application sees.
            void
            forget_hold( const grab::sequence::Command& command )
            {
                if( const auto* const press =
                        std::get_if<grab::sequence::PressCommand>( &command );
                    press != nullptr )
                {
                    std::erase( buttons_, press->button );
                    return;
                }
                if( const auto* const down =
                        std::get_if<grab::sequence::KeyDownCommand>( &command );
                    down != nullptr )
                {
                    std::erase( keys_, down->key );
                    return;
                }
                if( std::holds_alternative<grab::sequence::OverlayGrabCommand>(
                        command
                    ) )
                {
                    capture_held_ = false;
                }
            }

            // Is the explicit hold this step took STILL DOWN? The document's
            // own input.release / input.key_up retires it from these lists as
            // it runs, so a completed pair answers false and the unwind leaves
            // it alone.
            [[nodiscard]]
            bool
            still_outstanding( const grab::sequence::Command& command ) const
            {
                if( const auto* const press =
                        std::get_if<grab::sequence::PressCommand>( &command );
                    press != nullptr )
                {
                    return std::ranges::find( buttons_, press->button ) !=
                           buttons_.end();
                }
                if( const auto* const down =
                        std::get_if<grab::sequence::KeyDownCommand>( &command );
                    down != nullptr )
                {
                    return std::ranges::find( keys_, down->key ) != keys_.end();
                }
                return false;
            }

            // The seat calls behind release_holds(), spelled once so the
            // unwind path and the process-exit backstop cannot disagree about
            // what "release" means.
            [[nodiscard]]
            bool
            lift_document_hold( const grab::sequence::Command& command )
            {
                if constexpr( grab::kernel::sequence::PointerSeat<SeatT> )
                {
                    if( const auto* const press =
                            std::get_if<grab::sequence::PressCommand>( &command );
                        press != nullptr )
                    {
                        const bool up =
                            seat_->button( press->button, false ).has_value();
                        return seat_->flush().has_value() && up;
                    }
                }
                if constexpr( grab::kernel::sequence::KeyboardSeat<SeatT> )
                {
                    if( const auto* const down =
                            std::get_if<grab::sequence::KeyDownCommand>( &command );
                        down != nullptr )
                    {
                        return seat_->key_by_name( down->key, false ).has_value();
                    }
                }
                // A document_hold on anything else is a hold this runner does
                // not know how to lift, which is a defect rather than a
                // success to report.
                return false;
            }

            [[nodiscard]]
            bool
            lift_capture()
            {
                capture_held_ = false;
                if constexpr( grab::kernel::sequence::OverlaySeat<SeatT> )
                {
                    return seat_->overlay_release().has_value();
                }
                else
                {
                    // A seat with no overlay cannot have taken the pointer, so
                    // there is nothing to fail at.
                    return true;
                }
            }

            // When a Timed body next wants a tick. Purely a wake-up schedule:
            // the answer must not change with it, but without it a wait would
            // be pumped as fast as the loop can spin.
            template<typename PayloadT>
            [[nodiscard]]
            static std::optional<TimePoint>
            due_at( const PayloadT&,
                    const grab::kernel::sequence::CommandState& )
            {
                return std::nullopt;
            }

            [[nodiscard]]
            static std::optional<TimePoint>
            due_at( const grab::sequence::WaitCommand&,
                    const grab::kernel::sequence::CommandState& state )
            {
                return state.deadline;
            }

            [[nodiscard]]
            static std::optional<TimePoint>
            due_at( const grab::sequence::MoveCommand&          command,
                    const grab::kernel::sequence::CommandState& state )
            {
                return waypoint_due( command.options, state );
            }

            [[nodiscard]]
            static std::optional<TimePoint>
            due_at( const grab::sequence::FollowCommand&        command,
                    const grab::kernel::sequence::CommandState& state )
            {
                return waypoint_due( command.options, state );
            }

            [[nodiscard]]
            static std::optional<TimePoint>
            due_at( const grab::sequence::DragCommand&          command,
                    const grab::kernel::sequence::CommandState& state )
            {
                return waypoint_due( command.options, state );
            }

            [[nodiscard]]
            static std::optional<TimePoint>
            waypoint_due( const grab::input::DragOptions&             options,
                          const grab::kernel::sequence::CommandState& state )
            {
                if( state.emitted >= state.waypoints.size() )
                {
                    return std::nullopt;
                }
                const auto ordinal =
                    static_cast<std::chrono::milliseconds::rep>( state.emitted + 1U );
                return state.started + ( options.step_dwell * ordinal );
            }

            SeatT*                    seat_{ nullptr };
            std::vector<Entry>        entries_{};
            std::vector<std::uint8_t> buttons_{};
            std::vector<std::string>  keys_{};
            bool                      capture_held_{ false };
    };

}    // namespace grab::cli
