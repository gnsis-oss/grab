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
// nobody can test. The production CommandRunner (SeatRunner), the seat and
// the pump loop all live in the library now — kernel/sequence/runner.hpp,
// sequence/session_seat.hpp and kernel/sequence/drive.hpp — shared with the
// public grab/sequence.hpp API; the aliases below keep this header's old
// spellings working, and any seat satisfying the concepts in execute.hpp
// still qualifies, including grab::testing::RecordingSeat.

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
#include "kernel/sequence/runner.hpp"
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

    // The production CommandRunner and the RAII hold backstop now live in
    // the library (kernel/sequence/runner.hpp) so the public sequence API
    // (grab/sequence.hpp) and this CLI drive commands through exactly one
    // implementation. The aliases keep this header's old spellings working.
    template<typename SeatT>
    using SeatRunner = grab::kernel::sequence::SeatRunner<SeatT>;

    template<typename SeatT>
    using OutstandingHolds = grab::kernel::sequence::OutstandingHolds<SeatT>;

}    // namespace grab::cli
