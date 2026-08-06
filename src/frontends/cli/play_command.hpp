#pragma once

// `grab play` -- load a JSON sequence document, build it, and run it through
// the Player over a seat adapted from grab::Input.
//
//   grab play <sequence.json> [--pacing strict|grace|precise] [--grace-ms N]
//                             [--dry-run] [--report <path.jsonl>]
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

#include "grab/command.hpp"
#include "grab/command_descriptor.hpp"
#include "grab/result.hpp"
#include "grab/sequence_types.hpp"
#include "grab/trace.hpp"
#include "kernel/sequence/execute.hpp"
#include "kernel/sequence/player.hpp"
#include "kernel/sequence/sequence.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
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
    };

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
    [[nodiscard]]
    std::vector<std::string>
    report_records( const grab::kernel::sequence::Sequence& program,
                    const grab::kernel::sequence::Player&   player );

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
    [[nodiscard]]
    grab::Result<void>
    drive( grab::kernel::sequence::Player& player );

    // Build the player, drive it, write the report, and answer the process
    // exit code. Takes the runner rather than making one so the
    // failure-to-exit-code mapping is testable without a display.
    [[nodiscard]]
    int
    play_program( const grab::kernel::sequence::Sequence& program,
                  grab::kernel::sequence::CommandRunner&  runner,
                  const PlayOptions&                      options );

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
    //  - Player::unwind skips steps it has already exited (player.cpp:500),
    //    and a SUCCESSFUL input.press was already exited by succeed()
    //    (player.cpp:353). Its hold therefore survives the unwind. That is
    //    what release_outstanding() closes: the runner tracks explicit holds
    //    itself and lifts whatever is still down when the process is done.
    //    Without it a `grab play` that aborts after an input.press leaves a
    //    mouse button down for the next application, silently.
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
                entry->exited          = true;

                const bool held_before = entry->state.held || entry->state.document_hold;
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
                if( entry->state.held || entry->state.document_hold )
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

            // Lift anything the document left down. The process is the last
            // step: after it exits nothing can release a held button or key,
            // so an outstanding hold here is by definition one no later step
            // will lift.
            grab::NeutralizationOutcome
            release_outstanding()
            {
                if( buttons_.empty() && keys_.empty() )
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
                buttons_.clear();
                keys_.clear();
                return released ? grab::NeutralizationOutcome::Released
                                : grab::NeutralizationOutcome::Failed;
            }

            [[nodiscard]]
            std::size_t
            outstanding_holds() const noexcept
            {
                return buttons_.size() + keys_.size();
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
    };

}    // namespace grab::cli
