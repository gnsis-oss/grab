#include "grab/command.hpp"
#include "grab/command_descriptor.hpp"
#include "grab/ids.hpp"
#include "grab/result.hpp"
#include "grab/sequence_types.hpp"
#include "grab/trace.hpp"
#include "kernel/identity/id_factory.hpp"
#include "kernel/sequence/player.hpp"
#include "kernel/sequence/sequence.hpp"
#include "kernel/sequence/sequence_ops.hpp"
#include "kernel/support/log.hpp"
#include "kernel/support/log_tags.hpp"
#include "kernel/support/step_diag.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace grab::kernel::sequence
{

    namespace
    {

        using Clock     = std::chrono::steady_clock;
        using TimePoint = Clock::time_point;

        [[nodiscard]]
        bool
        terminal_status( grab::sequence::StepStatus status ) noexcept
        {
            return status ==
                   grab::sequence::StepStatus::Succeeded ||
                   status ==
                   grab::sequence::StepStatus::Failed ||
                   status == grab::sequence::StepStatus::Skipped;
        }

        // How a step is named in an error message: the author's label when
        // there is one, and its document position otherwise. A document may be
        // entirely unlabelled, so "step 'x'" alone would not locate a fault.
        [[nodiscard]]
        std::string
        step_name( const grab::sequence::Step& step )
        {
            if( !step.label.empty() )
            {
                std::string name{ "'" };
                name.append( step.label );
                name.append( "'" );
                return name;
            }
            std::string name{ "at index " };
            name.append( std::to_string( step.id.index() ) );
            return name;
        }

        [[nodiscard]]
        TimePoint
        add_grace( TimePoint                base,
                   std::chrono::nanoseconds grace ) noexcept
        {
            return base + std::chrono::duration_cast<Clock::duration>( grace );
        }

        // ── introspection helpers ──────────────────────────────
        //
        // Everything here is called from INSIDE a sink lambda, so none of it
        // runs unless the record is actually being emitted. Nothing allocates:
        // a log record built out of std::strings would be a per-step
        // allocation on the pump path, which is the one thing this facility
        // exists to keep out.
        //
        // THEY ARE ALL `[[maybe_unused]]`, AND THAT IS THE PROOF. At
        // `-DGRAB_LOG_LEVEL=off` every emitter lambda in this file is
        // discarded by `if constexpr` before it is instantiated, so nothing
        // references these at all and clang's
        // -Wunneeded-internal-declaration fires on each of them — under the
        // tree's global -Werror, loudly. The attribute is what lets the build
        // state that outcome instead of failing on it; it is not covering up
        // an unused helper, it is the shape of a helper that exists only to
        // feed records the ceiling threw away.

        constexpr std::size_t stepStatusSlots =
            static_cast<std::size_t>( grab::sequence::StepStatus::Count );

        [[nodiscard,
          maybe_unused]]
        constexpr std::string_view
        outcome_name( grab::NeutralizationOutcome outcome ) noexcept
        {
            switch( outcome )
            {
                case grab::NeutralizationOutcome::NotAttempted :
                    return "not_attempted";
                case grab::NeutralizationOutcome::NothingHeld :
                    return "nothing_held";
                case grab::NeutralizationOutcome::Released :
                    return "released";
                case grab::NeutralizationOutcome::Failed :
                    return "failed";
            }
            return "";
        }

        // A steady_clock time_point has an ARBITRARY EPOCH, so the only honest
        // thing to print is its own count — never a wall date. Microseconds
        // because a fabricated test clock starts at zero and a real one does
        // not fit a readable decimal in nanoseconds.
        [[nodiscard,
          maybe_unused]]
        std::int64_t
        stamp_us( TimePoint at ) noexcept
        {
            return static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    at.time_since_epoch()
                )
                    .count()
            );
        }

        [[nodiscard,
          maybe_unused]]
        std::int64_t
        micros( std::chrono::nanoseconds span ) noexcept
        {
            return static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>( span ).count()
            );
        }

        // The step's label as a VIEW. step_name() above builds a std::string
        // and belongs in error messages, which are already allocating; a log
        // line is not allowed to.
        [[nodiscard,
          maybe_unused]]
        std::string_view
        label_of( const grab::sequence::Step* step ) noexcept
        {
            return step == nullptr ? std::string_view{}
                                   : std::string_view{ step->label };
        }

        // TWO AXES, ONE DISPATCH: the pump phase and the CommandKind. Both
        // names are `constexpr std::string_view`s, so Instrument's
        // pointer-first lookup hits without a content compare and no string is
        // built per step.
        //
        // The kind is resolved inside an `if constexpr`, so a build whose
        // ceiling excludes the timers does not even visit the command variant.
        class Dispatch final
        {
            public:

                Dispatch( grab::diag::Instrument&     into,
                          std::string_view            phase_name,
                          const grab::sequence::Step& step ) noexcept :
                    phase_( into,
                            phase_name ),
                    op_( into,
                         kind_name( step ) )
                {
                }

            private:

                [[nodiscard]]
                static std::string_view
                kind_name( const grab::sequence::Step& step ) noexcept
                {
                    if constexpr( grab::diag::Measure<measureLevel>::enabled )
                    {
                        return grab::command_name(
                            grab::sequence::kind_of( step.command )
                        );
                    }
                    else
                    {
                        return {};
                    }
                }

                grab::diag::Measure<measureLevel> phase_;
                grab::diag::Measure<measureLevel> op_;
        };

    }    // namespace

    std::optional<std::chrono::nanoseconds>
    CommandRunner::declared_duration( const grab::sequence::Step& step ) const
    {
        // time.wait is the one op whose duration is mandatory in the document.
        // Everything else declares nothing by default, and nullopt means
        // UNKNOWN, SO MEASURE IT — never zero.
        const auto* const wait =
            std::get_if<grab::sequence::WaitCommand>( &step.command );
        if( wait == nullptr )
        {
            return std::nullopt;
        }
        return wait->duration;
    }

    std::chrono::nanoseconds
    grace_before( const grab::sequence::Step&   step,
                  grab::sequence::PacingOptions pacing ) noexcept
    {
        // Roots start immediately: grace is the gap BETWEEN steps, and a step
        // with no predecessor has nothing to be between.
        if( step.after.empty() )
        {
            return std::chrono::nanoseconds::zero();
        }
        switch( pacing.mode )
        {
            case grab::sequence::PacingMode::Strict :
                return std::chrono::nanoseconds::zero();
            case grab::sequence::PacingMode::Grace :
                return std::chrono::duration_cast<std::chrono::nanoseconds>(
                    pacing.grace
                );
            case grab::sequence::PacingMode::Precise :
                return std::chrono::duration_cast<std::chrono::nanoseconds>(
                    pacing.grace + step.extra_grace
                );
            case grab::sequence::PacingMode::Count :
                break;
        }
        return std::chrono::nanoseconds::zero();
    }

    grab::RetryClass
    retry_class_of_step( const grab::sequence::Step& step ) noexcept
    {
        const auto  kind  = grab::sequence::kind_of( step.command );
        const auto& table = grab::list_commands();
        const auto* descriptor =
            std::ranges::find( table, kind, &grab::CommandDescriptor::kind );
        return descriptor == table.end() ? grab::RetryClass::Never : descriptor->retry;
    }

    bool
    may_retry( const grab::sequence::Step& step,
               grab::ErrorCode             code ) noexcept
    {
        // A failed drag has a button down. Re-sending it would press a second
        // time while pretending the first did not happen, so this outranks
        // every RetryClass in the table.
        if( code == grab::ErrorCode::PossiblyCommitted )
        {
            return false;
        }
        // ResolveOnly needs a re-resolve the player cannot perform and
        // Compensated needs a compensation nothing implements, so only
        // Idempotent is retryable here. The column is read, never invented.
        return retry_class_of_step( step ) == grab::RetryClass::Idempotent;
    }

    Player::Player( const Sequence& program,
                    CommandRunner&  runner ) :
        program_( &program ),
        runner_( &runner ),
        status_( program.steps().size(),
                 grab::sequence::StepStatus::Pending ),
        runs_( program.steps().size() ),
        successor_offsets_( program.steps().size() + 1U,
                            0U )
    {
        const auto               steps = program.steps();
        const std::size_t        count = steps.size();

        // CSR successors, built once: the reverse of `after`, which is the
        // whole predecessor relation because the graph is built from it.
        std::vector<std::size_t> degrees( count, 0U );
        for( const auto& step : steps )
        {
            for( const auto predecessor : step.after )
            {
                const auto index = static_cast<std::size_t>( predecessor.index() );
                if( index < count )
                {
                    degrees[index] += 1U;
                }
            }
        }
        std::size_t total = 0U;
        for( std::size_t index = 0U; index < count; ++index )
        {
            successor_offsets_[index]  = total;
            total                     += degrees[index];
        }
        successor_offsets_[count] = total;
        successors_.resize( total );

        std::vector<std::size_t> cursor( count, 0U );
        for( std::size_t index = 0U; index < count; ++index )
        {
            cursor[index] = successor_offsets_[index];
        }
        for( const auto& step : steps )
        {
            for( const auto predecessor : step.after )
            {
                const auto index = static_cast<std::size_t>( predecessor.index() );
                if( index < count )
                {
                    successors_[cursor[index]]  = step.id;
                    cursor[index]              += 1U;
                }
            }
        }

        // Identity, not scheduling: minted once here so pump() stays free of
        // clock reads.
        run_id_ = grab::detail::next_operation_id().value;
    }

    std::size_t
    Player::index_of( grab::sequence::StepId id ) const
    {
        return static_cast<std::size_t>( id.index() );
    }

    bool
    Player::is_terminal( grab::sequence::StepId id ) const
    {
        const auto index = index_of( id );
        return index < status_.size() && terminal_status( status_[index] );
    }

    void
    Player::set_status( grab::sequence::StepId     id,
                        grab::sequence::StepStatus next,
                        TimePoint                  now )
    {
        const auto index = index_of( id );
        if( index >= status_.size() )
        {
            return;
        }
        const auto previous = status_[index];
        status_[index]      = next;
        if( previous == next )
        {
            return;
        }

        log::verbose(
            [this, id, index, previous, next, now]( auto& event )
            {
                event.tag( log::tags::player )
                    .value( "step", index )
                    .value( "label", label_of( program_->find( id ) ) )
                    .value( "from", grab::sequence::step_status_name( previous ) )
                    .value( "to", grab::sequence::step_status_name( next ) )
                    .value( "now_us", stamp_us( now ) );
            }
        );
    }

    void
    Player::set_state( grab::sequence::PlayState next )
    {
        if( state_ == next )
        {
            return;
        }
        const auto previous = state_;
        state_              = next;

        log::verbose(
            [this, previous, next]( auto& event )
            {
                event.tag( log::tags::player )
                    .value( "state_from", grab::sequence::play_state_name( previous ) )
                    .value( "state_to", grab::sequence::play_state_name( next ) )
                    .value( "frontier", frontier_.size() )
                    .value( "now_us", stamp_us( last_now_ ) );
            }
        );
    }

    void
    Player::admit_to_frontier( grab::sequence::StepId id,
                               TimePoint              ready_at )
    {
        const auto index = index_of( id );
        if( index < runs_.size() )
        {
            runs_[index].ready_at = ready_at;
        }
        frontier_.push_back( id );
        deepest_frontier_ = std::max( deepest_frontier_, frontier_.size() );

        log::verbose(
            [this, id, index, ready_at]( auto& event )
            {
                event.tag( log::tags::player )
                    .value( "admitted", index )
                    .value( "label", label_of( program_->find( id ) ) )
                    .value( "ready_us", stamp_us( ready_at ) )
                    .value( "frontier", frontier_.size() );
            }
        );
    }

    void
    Player::drop_from_frontier( grab::sequence::StepId id )
    {
        const auto found = std::ranges::find( frontier_, id );
        if( found == frontier_.end() )
        {
            return;
        }
        frontier_.erase( found );

        log::verbose(
            [this, id]( auto& event )
            {
                event.tag( log::tags::player )
                    .value( "retired", index_of( id ) )
                    .value( "label", label_of( program_->find( id ) ) )
                    .value( "status",
                            grab::sequence::step_status_name( status_of( id ) ) )
                    .value( "frontier", frontier_.size() );
            }
        );
    }

    grab::NeutralizationOutcome
    Player::neutralize( grab::sequence::StepId id,
                        TimePoint              now )
    {
        const auto index = index_of( id );
        if( index >= runs_.size() )
        {
            return grab::NeutralizationOutcome::NotAttempted;
        }
        auto& run = runs_[index];
        if( !run.entered || run.exited )
        {
            return grab::NeutralizationOutcome::NotAttempted;
        }
        const auto* const step = program_->find( id );
        if( step == nullptr )
        {
            return grab::NeutralizationOutcome::NotAttempted;
        }

        // A Blocking body runs on a worker, so join it before exit(). It costs
        // the rest of that step; abandoning the worker would race exit()
        // against a capture still writing its buffer.
        if( grab::is_blocking_command( grab::sequence::kind_of( step->command ) ) )
        {
            runner_->join( *step );
        }
        grab::NeutralizationOutcome outcome{ grab::NeutralizationOutcome::NotAttempted };
        {
            const Dispatch timer{ instrument_, phase::playExit, *step };
            outcome = runner_->exit( *step, now );
        }
        run.exited = true;

        if( outcome == grab::NeutralizationOutcome::Failed )
        {
            neutralization_ = grab::NeutralizationOutcome::Failed;
        }
        else if( outcome ==
                 grab::NeutralizationOutcome::Released &&
                 neutralization_ != grab::NeutralizationOutcome::Failed )
        {
            neutralization_ = grab::NeutralizationOutcome::Released;
        }
        else if( neutralization_ == grab::NeutralizationOutcome::NotAttempted )
        {
            neutralization_ = grab::NeutralizationOutcome::NothingHeld;
        }

        log::verbose(
            [step, outcome, now]( auto& event )
            {
                event.tag( log::tags::player )
                    .value( "neutralized", step->id.index() )
                    .value( "label", label_of( step ) )
                    .value( "outcome", outcome_name( outcome ) )
                    .value( "released",
                            outcome == grab::NeutralizationOutcome::Released )
                    .value( "now_us", stamp_us( now ) );
            }
        );
        return outcome;
    }

    void
    Player::record_neutralization( grab::NeutralizationOutcome outcome ) noexcept
    {
        switch( outcome )
        {
            case grab::NeutralizationOutcome::Failed :
                neutralization_ = grab::NeutralizationOutcome::Failed;
                return;
            case grab::NeutralizationOutcome::Released :
                if( neutralization_ != grab::NeutralizationOutcome::Failed )
                {
                    neutralization_ = grab::NeutralizationOutcome::Released;
                }
                return;
            case grab::NeutralizationOutcome::NothingHeld :
                if( neutralization_ == grab::NeutralizationOutcome::NotAttempted )
                {
                    neutralization_ = grab::NeutralizationOutcome::NothingHeld;
                }
                return;
            case grab::NeutralizationOutcome::NotAttempted :
                // No information: the step held nothing to begin with, which
                // must not turn a clean run's report into a neutralization.
                return;
        }
    }

    grab::NeutralizationOutcome
    Player::reap_holds( grab::sequence::StepId id )
    {
        const auto* const step = program_->find( id );
        if( step == nullptr )
        {
            return grab::NeutralizationOutcome::NotAttempted;
        }
        const auto outcome = runner_->release_holds( *step );
        record_neutralization( outcome );

        log::verbose(
            [step, outcome]( auto& event )
            {
                event.tag( log::tags::player )
                    .value( "reaped", step->id.index() )
                    .value( "label", label_of( step ) )
                    .value( "outcome", outcome_name( outcome ) )
                    .value( "released",
                            outcome == grab::NeutralizationOutcome::Released );
            }
        );
        return outcome;
    }

    void
    Player::admit_successors( grab::sequence::StepId id,
                              TimePoint              now )
    {
        const auto index = index_of( id );
        if( index + 1U >= successor_offsets_.size() )
        {
            return;
        }
        const auto pacing = program_->pacing();
        for( auto slot = successor_offsets_[index];
             slot < successor_offsets_[index + 1U];
             ++slot )
        {
            const auto successor       = successors_[slot];
            const auto successor_index = index_of( successor );
            if( successor_index >=
                status_.size() ||
                status_[successor_index] != grab::sequence::StepStatus::Pending )
            {
                continue;
            }
            const auto* const step = program_->find( successor );
            if( step == nullptr )
            {
                continue;
            }

            // Eligible when every predecessor is terminal, at the LATEST of
            // their completions. The next deadline comes from actual
            // completion, never from an absolute schedule — the pump does not
            // catch up.
            TimePoint eligible{};
            bool      ready   = true;
            auto      blocker = grab::sequence::StepId{};
            for( const auto predecessor : step->after )
            {
                if( !is_terminal( predecessor ) )
                {
                    ready   = false;
                    blocker = predecessor;
                    break;
                }
                eligible =
                    std::max( eligible, runs_[index_of( predecessor )].finished_at );
            }
            if( !ready )
            {
                // "Blocked" with no reason is the log line people curse at, so
                // this names the predecessor that is still non-terminal and
                // what it is doing instead.
                log::verbose(
                    [this, successor, blocker]( auto& event )
                    {
                        event.tag( log::tags::player )
                            .value( "blocked", index_of( successor ) )
                            .value( "label", label_of( program_->find( successor ) ) )
                            .value( "waiting_on", index_of( blocker ) )
                            .value( "waiting_on_label",
                                    label_of( program_->find( blocker ) ) )
                            .value(
                                "waiting_on_status",
                                grab::sequence::step_status_name( status_of( blocker ) )
                            );
                    }
                );
                continue;
            }

            const auto grace    = grace_before( *step, pacing );
            const auto ready_at = add_grace( eligible, grace );
            set_status( successor, grab::sequence::StepStatus::Ready, now );
            admit_to_frontier( successor, ready_at );

            if( grace > std::chrono::nanoseconds::zero() )
            {
                // Grace ACTUALLY APPLIED, with the mode that authorised it:
                // extra_grace is read only under Precise, so a report that
                // printed the step's own field unconditionally would explain a
                // delay that never happened.
                log::verbose(
                    [this, successor, step, pacing, grace, ready_at]( auto& event )
                    {
                        const auto extra =
                            pacing.mode == grab::sequence::PacingMode::Precise
                                ? micros( step->extra_grace )
                                : std::int64_t{ 0 };
                        event.tag( log::tags::player )
                            .value( "graced", index_of( successor ) )
                            .value( "label", label_of( step ) )
                            .value( "mode",
                                    grab::sequence::pacing_mode_name( pacing.mode ) )
                            .value( "base_grace_us", micros( pacing.grace ) )
                            .value( "extra_grace_us", extra )
                            .value( "total_grace_us", micros( grace ) )
                            .value( "ready_us", stamp_us( ready_at ) );
                    }
                );
            }
        }
    }

    void
    Player::succeed( grab::sequence::StepId id,
                     TimePoint              now )
    {
        const auto        index = index_of( id );
        auto&             run   = runs_[index];
        const auto* const step  = program_->find( id );

        run.call_duration       = now - run.entered_at;
        run.finished_at         = now;
        if( step != nullptr && run.entered && !run.exited )
        {
            // A clean completion still exits — that is what releases anything
            // the body holds — but it is not neutralization, so the run keeps
            // reporting NotAttempted.
            const Dispatch timer{ instrument_, phase::playExit, *step };
            ( void )runner_->exit( *step, now );
            run.exited = true;
        }
        set_status( id, grab::sequence::StepStatus::Succeeded, now );
        last_finish_ = std::max( last_finish_, now );
        drop_from_frontier( id );
        admit_successors( id, now );
    }

    void
    Player::fail_step( grab::sequence::StepId id,
                       grab::ErrorCode        code,
                       TimePoint              now )
    {
        const auto        index = index_of( id );
        auto&             run   = runs_[index];
        const auto* const step  = program_->find( id );

        run.call_duration       = now - run.entered_at;
        run.finished_at         = now;
        ( void )neutralize( id, now );
        set_status( id, grab::sequence::StepStatus::Failed, now );
        last_finish_ = std::max( last_finish_, now );
        drop_from_frontier( id );

        if( step == nullptr )
        {
            return;
        }

        log::nominal(
            [&step, code]( auto& event )
            {
                event.tag( log::tags::player )
                    .value( "failed", step->id.index() )
                    .value( "error", grab::name_of( code ) )
                    .value( "on_error",
                            grab::sequence::error_policy_name( step->on_error ) );
            }
        );

        switch( step->on_error )
        {
            case grab::sequence::ErrorPolicy::Continue :
                admit_successors( id, now );
                return;

            case grab::sequence::ErrorPolicy::Goto :
                {
                    const auto target = program_->resolve_label( step->on_error_target );
                    if( !target.has_value() )
                    {
                        std::string message{ "step " };
                        message.append( step_name( *step ) );
                        message.append( " recovers to label '" );
                        message.append( step->on_error_target );
                        message.append( "', which names no step" );
                        failure_ =
                            grab::fail( grab::ErrorCode::NoMatch, std::move( message ) )
                                .error();
                        unwind( now );
                        set_state( grab::sequence::PlayState::Interrupted );
                        return;
                    }
                    auto jumped = jump_to( *target );
                    if( !jumped.has_value() )
                    {
                        failure_ = jumped.error();
                        unwind( now );
                        set_state( grab::sequence::PlayState::Interrupted );
                    }
                    return;
                }

            case grab::sequence::ErrorPolicy::Abort :
            case grab::sequence::ErrorPolicy::Count :
                break;
        }

        // Abort and interrupt() share ONE unwind path.
        std::string message{ "step " };
        message.append( step_name( *step ) );
        message.append( " failed and its policy is abort" );
        failure_ = grab::fail( code, std::move( message ) ).error();
        unwind( now );
        set_state( grab::sequence::PlayState::Interrupted );
    }

    void
    Player::on_status( grab::sequence::StepId id,
                       grab::sequence::Status status,
                       TimePoint              now )
    {
        if( status == grab::sequence::Status::Running )
        {
            return;
        }
        if( status == grab::sequence::Status::Success )
        {
            succeed( id, now );
            return;
        }

        const auto        index = index_of( id );
        auto&             run   = runs_[index];
        const auto* const step  = program_->find( id );
        if( step == nullptr )
        {
            fail_step( id, grab::ErrorCode::InternalFault, now );
            return;
        }

        auto       code = runner_->last_error( *step );
        const bool retrying =
            may_retry( *step, code ) && run.retries < maxRetriesPerStep;

        // The RetryClass COLUMN is what was consulted, so it is what the log
        // names. It is resolved inside the sink so a run with logging off does
        // not walk the descriptor table on its failure path.
        log::verbose(
            [step, code, retrying, &run]( auto& event )
            {
                event.tag( log::tags::player )
                    .value( "retry_decision", step->id.index() )
                    .value( "label", label_of( step ) )
                    .value( "error", grab::name_of( code ) )
                    .value( "retry_class",
                            grab::detail::retry_class_name
                                .text_of( retry_class_of_step( *step ), "" ) )
                    .value( "attempts", run.retries )
                    .value( "limit", maxRetriesPerStep )
                    .value( "verdict", retrying ? "retry" : "give_up" );
            }
        );

        if( retrying )
        {
            run.retries += 1U;
            ( void )neutralize( id, now );
            run.exited     = false;
            run.entered_at = now;
            auto again     = grab::sequence::Status::Running;
            {
                const Dispatch timer{ instrument_, phase::playEnter, *step };
                again = runner_->enter( *step, now );
            }
            if( again == grab::sequence::Status::Running )
            {
                return;
            }
            if( again == grab::sequence::Status::Success )
            {
                succeed( id, now );
                return;
            }
            code = runner_->last_error( *step );
        }
        fail_step( id, code, now );
    }

    void
    Player::unwind( TimePoint now )
    {
        log::verbose(
            [this, now]( auto& event )
            {
                event.tag( log::tags::player )
                    .value( "unwind", program_->name() )
                    .value( "entered", entry_order_.size() )
                    .value( "frontier", frontier_.size() )
                    .value( "now_us", stamp_us( now ) );
            }
        );

        // The order IS the contract, so it is logged as an ordinal rather than
        // left to be inferred from the record order of a log that may be
        // interleaved with another thread's.
        std::size_t reaped = 0U;

        // Reverse entry order, so a step that pressed on top of another
        // releases first.
        for( std::size_t position = entry_order_.size(); position-- > 0U; )
        {
            const auto id    = entry_order_[position];
            const auto index = index_of( id );
            if( index >= runs_.size() )
            {
                continue;
            }
            if( !runs_[index].entered )
            {
                continue;
            }
            reaped += 1U;
            if( runs_[index].exited )
            {
                // succeed() exits a cleanly-completed step, so neutralize()
                // can never revisit it — and an EXPLICIT, document-owned hold
                // is still down, waiting for a later step this unwind has just
                // cancelled. release_holds is the seam that lifts it, and it
                // is deliberately not a second exit(): that contract is
                // exactly-once, and calling it twice would double-release the
                // implicit case.
                const auto outcome = reap_holds( id );
                log::verbose(
                    [this, id, index, reaped, outcome]( auto& event )
                    {
                        event.tag( log::tags::player )
                            .value( "unwound", index )
                            .value( "label", label_of( program_->find( id ) ) )
                            .value( "order", reaped )
                            .value( "path", "release_holds" )
                            .value( "released",
                                    outcome == grab::NeutralizationOutcome::Released )
                            .value( "outcome", outcome_name( outcome ) );
                    }
                );
                continue;
            }
            runs_[index].call_duration = now - runs_[index].entered_at;
            const auto outcome         = neutralize( id, now );
            log::verbose(
                [this, id, index, reaped, outcome]( auto& event )
                {
                    event.tag( log::tags::player )
                        .value( "unwound", index )
                        .value( "label", label_of( program_->find( id ) ) )
                        .value( "order", reaped )
                        .value( "path", "exit" )
                        .value( "released",
                                outcome == grab::NeutralizationOutcome::Released )
                        .value( "outcome", outcome_name( outcome ) );
                }
            );
            if( !terminal_status( status_[index] ) )
            {
                set_status( id, grab::sequence::StepStatus::Skipped, now );
                runs_[index].finished_at = now;
            }
        }

        // Anything still merely Ready never entered, so there is nothing to
        // release; it is simply not going to run.
        for( const auto id : frontier_ )
        {
            const auto index = index_of( id );
            if( index < status_.size() && !terminal_status( status_[index] ) )
            {
                set_status( id, grab::sequence::StepStatus::Skipped, now );
                runs_[index].finished_at = now;
            }
        }
        frontier_.clear();
    }

    void
    Player::report_run()
    {
        // Once per run, and only for a run that actually ended. Called from
        // the tail of pump(), interrupt() and skip() rather than from
        // set_state(), because a summary emitted from inside the transition
        // would print a `pump` tally missing the very pump that produced it.
        if( reported_ )
        {
            return;
        }
        if( state_ !=
            grab::sequence::PlayState::Done &&
            state_ != grab::sequence::PlayState::Interrupted )
        {
            return;
        }
        reported_ = true;

        log::nominal(
            [this]( auto& event )
            {
                std::array<std::size_t, stepStatusSlots> counts{};
                for( const auto status : status_ )
                {
                    const auto slot = static_cast<std::size_t>( status );
                    if( slot < counts.size() )
                    {
                        counts[slot] += 1U;
                    }
                }

                // The longest single step is the first thing anyone wants
                // after "how long did it take", and the label is what makes it
                // actionable — an index alone sends the reader back to the
                // document to find out what it was.
                std::chrono::nanoseconds longest{};
                std::string_view         longest_label{};
                std::size_t              longest_step = 0U;
                for( const auto& step : program_->steps() )
                {
                    const auto slot = index_of( step.id );
                    if( slot >= runs_.size() || runs_[slot].call_duration <= longest )
                    {
                        continue;
                    }
                    longest       = runs_[slot].call_duration;
                    longest_step  = slot;
                    longest_label = step.label;
                }

                event.tag( log::tags::player )
                    .value( "summary", program_->name() )
                    .value( "state", grab::sequence::play_state_name( state_ ) )
                    .value( "steps", status_.size() )
                    .value( "succeeded",
                            counts[static_cast<std::size_t>(
                                grab::sequence::StepStatus::Succeeded
                            )] )
                    .value( "failed",
                            counts[static_cast<std::size_t>(
                                grab::sequence::StepStatus::Failed
                            )] )
                    .value( "skipped",
                            counts[static_cast<std::size_t>(
                                grab::sequence::StepStatus::Skipped
                            )] )
                    .value( "pending",
                            counts[static_cast<std::size_t>(
                                grab::sequence::StepStatus::Pending
                            )] )
                    .value( "ready",
                            counts[static_cast<std::size_t>(
                                grab::sequence::StepStatus::Ready
                            )] )
                    .value( "running",
                            counts[static_cast<std::size_t>(
                                grab::sequence::StepStatus::Running
                            )] )
                    // planned() is the critical path over DECLARED durations
                    // and grace; steps that declare nothing contribute zero to
                    // it, so it is a floor and never a forecast. Against
                    // elapsed_us it is the whole point of the pair: a run far
                    // past its floor was waiting on the application, not on
                    // the schedule.
                    //
                    // It is computed HERE, inside the sink, so a run with
                    // logging off never walks the graph for it. The cost of
                    // that placement is one `ops.planned` tally in the LOAD
                    // instrument per summarised run — an observer effect, and
                    // small, but real: that instrument's call count for
                    // `ops.planned` is one higher when player logging is on.
                    .value( "planned_us", micros( planned( *program_ ) ) )
                    .value( "elapsed_us", micros( elapsed() ) )
                    .value( "longest_step", longest_step )
                    .value( "longest_label", longest_label )
                    .value( "longest_us", micros( longest ) )
                    .value( "deepest_frontier", deepest_frontier_ )
                    .value( "neutralization", outcome_name( neutralization_ ) )
                    // An instrument that dropped a name has stopped being a
                    // full accounting, and a report that did not say so would
                    // be read as one.
                    .value( "instrument",
                            instrument_.overflowed() ? "overflowed_incomplete"
                                                     : "complete" )
                    .value( "phase_unit", "ns" );

                // Per NAME, never summed: the phase axis and the CommandKind
                // axis both time the same dispatches, so a total over every
                // tally would double-count by construction.
                for( const auto& tally : instrument_.tallies() )
                {
                    event.value( tally.name, tally.total.count() );
                }
            }
        );
    }

    grab::Result<void>
    Player::jump_to( grab::sequence::StepId target )
    {
        const auto* const step = program_->find( target );
        if( step == nullptr )
        {
            std::string message{ "no step with index " };
            message.append( std::to_string( target.index() ) );
            return grab::fail( grab::ErrorCode::NoMatch, message );
        }
        if( is_terminal( target ) )
        {
            std::string message{ "goto is forward-only; step " };
            message.append( step_name( *step ) );
            message.append( " has already run" );
            return grab::fail( grab::ErrorCode::InvalidArgument, message );
        }

        // Forward-only. Backward motion means re-running effects, which is a
        // separate feature needing its own design.
        //
        // In the current design the terminal check above already catches every
        // reachable backward jump: a step can only be in the frontier once all
        // its predecessors are terminal, and terminality propagates up the
        // ancestor chain. This second check is therefore defensive — it stops
        // a frontier placed by a previous goto_step, which bypasses
        // eligibility, from being jumped behind.
        for( const auto member : frontier_ )
        {
            if( member == target )
            {
                continue;
            }
            const auto ancestors = program_->ancestors_of( member );
            if( std::ranges::find( ancestors, target ) != ancestors.end() )
            {
                const auto* const blocker = program_->find( member );
                std::string       message{ "goto is forward-only; step " };
                message.append( step_name( *step ) );
                message.append( " is an ancestor of step " );
                message.append( blocker == nullptr ? std::string{ "in the frontier" }
                                                   : step_name( *blocker ) );
                return grab::fail( grab::ErrorCode::InvalidArgument, message );
            }
        }

        const auto now = last_now_;

        // Every ancestor that has not already run is skipped. Ancestors, not a
        // prefix of order(): order() is one arbitrary linearization, and
        // skipping its prefix would skip unrelated parallel branches.
        for( const auto ancestor : program_->ancestors_of( target ) )
        {
            const auto index = index_of( ancestor );
            if( index >= status_.size() || terminal_status( status_[index] ) )
            {
                continue;
            }
            if( runs_[index].entered && !runs_[index].exited )
            {
                runs_[index].call_duration = now - runs_[index].entered_at;
                ( void )neutralize( ancestor, now );
            }
            set_status( ancestor, grab::sequence::StepStatus::Skipped, now );
            runs_[index].finished_at = now;
        }

        // The rest of the frontier is dropped. A member that was entered has
        // to be exited — it may hold a button — and is terminal afterwards; one
        // that never entered goes back to Pending and is simply not waited on.
        for( const auto member : frontier_ )
        {
            const auto index = index_of( member );
            if( member ==
                target ||
                index >=
                status_.size() ||
                terminal_status( status_[index] ) )
            {
                continue;
            }
            if( runs_[index].entered && !runs_[index].exited )
            {
                runs_[index].call_duration = now - runs_[index].entered_at;
                ( void )neutralize( member, now );
                set_status( member, grab::sequence::StepStatus::Skipped, now );
                runs_[index].finished_at = now;
                continue;
            }
            set_status( member, grab::sequence::StepStatus::Pending, now );
        }

        frontier_.clear();

        const auto target_index = index_of( target );
        if( status_[target_index] != grab::sequence::StepStatus::Running )
        {
            set_status( target, grab::sequence::StepStatus::Ready, now );
            admit_to_frontier( target,
                               add_grace( now,
                                          grace_before( *step, program_->pacing() ) ) );
        }
        else
        {
            admit_to_frontier( target, runs_[target_index].ready_at );
        }

        log::nominal(
            [step, now]( auto& event )
            {
                event.tag( log::tags::player )
                    .value( "goto", step->id.index() )
                    .value( "label", label_of( step ) )
                    .value( "now_us", stamp_us( now ) );
            }
        );
        return {};
    }

    grab::Result<void>
    Player::play()
    {
        if( state_ == grab::sequence::PlayState::Paused )
        {
            set_state( grab::sequence::PlayState::Playing );
            return {};
        }
        if( state_ == grab::sequence::PlayState::Playing )
        {
            return {};
        }
        if( state_ != grab::sequence::PlayState::Idle )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "this run has already finished; construct a new "
                               "player to replay the document" );
        }

        set_state( grab::sequence::PlayState::Playing );
        for( const auto& step : program_->steps() )
        {
            if( !step.after.empty() )
            {
                continue;
            }
            set_status( step.id, grab::sequence::StepStatus::Ready, last_now_ );
            // Roots start immediately: grace is the gap between steps.
            admit_to_frontier( step.id, last_now_ );
        }

        log::nominal(
            [this]( auto& event )
            {
                event.tag( log::tags::player )
                    .value( "play", program_->name() )
                    .value( "roots", frontier_.size() )
                    .value(
                        "pacing",
                        grab::sequence::pacing_mode_name( program_->pacing().mode )
                    );
            }
        );
        return {};
    }

    grab::Result<void>
    Player::pause()
    {
        if( state_ != grab::sequence::PlayState::Playing )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "nothing is playing, so there is nothing to pause" );
        }
        set_state( grab::sequence::PlayState::Paused );
        return {};
    }

    grab::Result<void>
    Player::interrupt()
    {
        if( state_ !=
            grab::sequence::PlayState::Playing &&
            state_ != grab::sequence::PlayState::Paused )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "nothing is playing, so there is nothing to "
                               "interrupt" );
        }
        // Grace does NOT apply here: a held button must not wait out a grace
        // period before being released.
        unwind( last_now_ );
        set_state( grab::sequence::PlayState::Interrupted );
        report_run();
        return {};
    }

    grab::Result<void>
    Player::skip()
    {
        if( state_ !=
            grab::sequence::PlayState::Playing &&
            state_ != grab::sequence::PlayState::Paused )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "nothing is playing, so there is nothing to skip" );
        }

        const auto now = last_now_;
        scratch_.assign( frontier_.begin(), frontier_.end() );
        frontier_.clear();

        for( const auto id : scratch_ )
        {
            const auto index = index_of( id );
            if( index >= status_.size() )
            {
                continue;
            }
            // A step that is already Running has been ENTERED, so exit() runs
            // on it: skipping never strands a held button.
            if( runs_[index].entered && !runs_[index].exited )
            {
                runs_[index].call_duration = now - runs_[index].entered_at;
                ( void )neutralize( id, now );
            }
            set_status( id, grab::sequence::StepStatus::Skipped, now );
            runs_[index].finished_at = now;
        }
        for( const auto id : scratch_ )
        {
            admit_successors( id, now );
        }
        if( state_ == grab::sequence::PlayState::Playing && frontier_.empty() )
        {
            set_state( grab::sequence::PlayState::Done );
        }
        report_run();
        return {};
    }

    grab::Result<void>
    Player::goto_step( grab::sequence::StepId target )
    {
        if( state_ !=
            grab::sequence::PlayState::Playing &&
            state_ != grab::sequence::PlayState::Paused )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "nothing is playing, so there is nowhere to go "
                               "from" );
        }
        return jump_to( target );
    }

    grab::Result<void>
    Player::goto_label( std::string_view label )
    {
        const auto target = program_->resolve_label( label );
        if( !target.has_value() )
        {
            // A label that resolves to nothing is an error, not a silent
            // no-op.
            std::string message{ "no step is labelled '" };
            message.append( label );
            message.append( "'" );
            return grab::fail( grab::ErrorCode::NoMatch, message );
        }
        return goto_step( *target );
    }

    grab::Result<void>
    Player::pump( TimePoint now )
    {
        last_now_ = now;
        if( state_ !=
            grab::sequence::PlayState::Playing &&
            state_ != grab::sequence::PlayState::Paused )
        {
            return {};
        }

        const bool failed_before = failure_.has_value();

        {
            // The whole call, so `pump - (enter + tick + exit)` is what the
            // scheduler spent on its own behalf. It closes before the Done
            // check below, which is what lets the run summary print a `pump`
            // tally that already includes the pump that ended the run.
            const diag::Measure<measureLevel> pump_timer{ instrument_, phase::playPump };

            // Bounded because every iteration that makes progress moves at
            // least one step through enter or through completion, and each
            // step does both at most once per retry.
            const std::size_t                 limit = ( 2U * status_.size() ) + 2U;
            for( std::size_t iteration = 0U; iteration < limit; ++iteration )
            {
                bool progressed = false;

                scratch_.assign( frontier_.begin(), frontier_.end() );
                for( const auto id : scratch_ )
                {
                    const auto index = index_of( id );
                    if( index >=
                        status_.size() ||
                        status_[index] != grab::sequence::StepStatus::Running )
                    {
                        continue;
                    }
                    const auto* const step = program_->find( id );
                    if( step == nullptr )
                    {
                        continue;
                    }
                    const auto due = runner_->next_tick( *step );
                    if( due.has_value() && *due > now )
                    {
                        continue;
                    }
                    auto status = grab::sequence::Status::Running;
                    {
                        const Dispatch timer{ instrument_, phase::playTick, *step };
                        status = runner_->tick( *step, now );
                    }
                    if( status != grab::sequence::Status::Running )
                    {
                        on_status( id, status, now );
                        progressed = true;
                    }
                    if( state_ !=
                        grab::sequence::PlayState::Playing &&
                        state_ != grab::sequence::PlayState::Paused )
                    {
                        break;
                    }
                }

                // Paused admits NO new steps, but the running ones above still
                // run to completion: pausing mid-drag would strand a held
                // button.
                if( state_ == grab::sequence::PlayState::Playing )
                {
                    scratch_.assign( frontier_.begin(), frontier_.end() );
                    for( const auto id : scratch_ )
                    {
                        const grab::sequence::Step* step = nullptr;
                        {
                            // The ADMISSION DECISION, and nothing else: this
                            // scope closes before enter() so the two are
                            // separable in the report. Every `continue` below
                            // leaves it through the destructor, so a rejected
                            // candidate is still charged for the look.
                            const diag::Measure<measureLevel> scan_timer{
                                instrument_,
                                phase::playReadyScan,
                            };

                            const auto index = index_of( id );
                            if( index >=
                                status_.size() ||
                                status_[index] !=
                                grab::sequence::StepStatus::Ready ||
                                runs_[index].ready_at > now )
                            {
                                continue;
                            }
                            step = program_->find( id );
                            if( step == nullptr )
                            {
                                continue;
                            }

                            set_status( id, grab::sequence::StepStatus::Running, now );
                            runs_[index].entered    = true;
                            runs_[index].exited     = false;
                            runs_[index].entered_at = now;
                            runs_[index].declared = runner_->declared_duration( *step );
                            entry_order_.push_back( id );
                            if( !anything_entered_ )
                            {
                                anything_entered_ = true;
                                first_entry_      = now;
                            }
                            progressed = true;
                        }

                        auto status = grab::sequence::Status::Running;
                        {
                            const Dispatch timer{ instrument_, phase::playEnter, *step };
                            status = runner_->enter( *step, now );
                        }
                        on_status( id, status, now );

                        if( state_ != grab::sequence::PlayState::Playing )
                        {
                            break;
                        }
                    }
                }

                if( !progressed )
                {
                    break;
                }
                if( state_ !=
                    grab::sequence::PlayState::Playing &&
                    state_ != grab::sequence::PlayState::Paused )
                {
                    break;
                }
            }
        }

        // A run reaches Done when the FRONTIER EMPTIES, not when every step is
        // terminal: goto leaves unrelated branches Pending and does not wait
        // on them.
        if( state_ == grab::sequence::PlayState::Playing && frontier_.empty() )
        {
            set_state( grab::sequence::PlayState::Done );
        }
        report_run();

        if( failure_.has_value() && !failed_before )
        {
            return std::unexpected( *failure_ );
        }
        return {};
    }

    grab::sequence::PlayState
    Player::state() const noexcept
    {
        return state_;
    }

    grab::sequence::StepStatus
    Player::status_of( grab::sequence::StepId id ) const
    {
        const auto index = index_of( id );
        if( id.is_nil() || index >= status_.size() )
        {
            return grab::sequence::StepStatus::Pending;
        }
        return status_[index];
    }

    std::span<const grab::sequence::StepId>
    Player::frontier() const noexcept
    {
        return std::span<const grab::sequence::StepId>{ frontier_ };
    }

    std::span<const grab::sequence::StepId>
    Player::entry_order() const noexcept
    {
        return std::span<const grab::sequence::StepId>{ entry_order_ };
    }

    std::optional<TimePoint>
    Player::next_deadline() const
    {
        if( state_ !=
            grab::sequence::PlayState::Playing &&
            state_ != grab::sequence::PlayState::Paused )
        {
            return std::nullopt;
        }

        std::optional<TimePoint> best;
        for( const auto id : frontier_ )
        {
            const auto index = index_of( id );
            if( index >= status_.size() )
            {
                continue;
            }
            std::optional<TimePoint> candidate;
            if( status_[index] == grab::sequence::StepStatus::Ready )
            {
                if( state_ != grab::sequence::PlayState::Playing )
                {
                    continue;
                }
                candidate = runs_[index].ready_at;
            }
            else if( status_[index] == grab::sequence::StepStatus::Running )
            {
                const auto* const step = program_->find( id );
                if( step == nullptr )
                {
                    continue;
                }
                candidate = runner_->next_tick( *step );
                if( !candidate.has_value() )
                {
                    // No stated deadline means "tick me every pump".
                    candidate = last_now_;
                }
            }
            if( candidate.has_value() && ( !best.has_value() || *candidate < *best ) )
            {
                best = candidate;
            }
        }
        return best;
    }

    std::optional<TimePoint>
    Player::ready_at( grab::sequence::StepId id ) const
    {
        const auto index = index_of( id );
        if( index >=
            runs_.size() ||
            status_[index] == grab::sequence::StepStatus::Pending )
        {
            return std::nullopt;
        }
        return runs_[index].ready_at;
    }

    std::optional<TimePoint>
    Player::entered_at( grab::sequence::StepId id ) const
    {
        const auto index = index_of( id );
        if( index >= runs_.size() || !runs_[index].entered )
        {
            return std::nullopt;
        }
        return runs_[index].entered_at;
    }

    std::optional<TimePoint>
    Player::finished_at( grab::sequence::StepId id ) const
    {
        const auto index = index_of( id );
        if( index >= runs_.size() || !terminal_status( status_[index] ) )
        {
            return std::nullopt;
        }
        return runs_[index].finished_at;
    }

    std::chrono::nanoseconds
    Player::elapsed() const noexcept
    {
        if( !anything_entered_ || last_finish_ < first_entry_ )
        {
            return std::chrono::nanoseconds::zero();
        }
        return std::chrono::duration_cast<std::chrono::nanoseconds>( last_finish_ -
                                                                     first_entry_ );
    }

    grab::sequence::StepTiming
    Player::timing_of( grab::sequence::StepId id ) const
    {
        const auto index = index_of( id );
        if( index >= runs_.size() )
        {
            return {};
        }
        // server_observed stays empty here: an X-server millisecond counter
        // shares no origin with steady_clock, and the two must never be
        // subtracted from each other.
        return grab::sequence::StepTiming{
            .declared        = runs_[index].declared,
            .call_duration   = runs_[index].call_duration,
            .server_observed = std::nullopt,
        };
    }

    std::chrono::nanoseconds
    Player::overrun_of( grab::sequence::StepId id ) const
    {
        const auto index = index_of( id );
        if( index >= runs_.size() || !runs_[index].declared.has_value() )
        {
            return std::chrono::nanoseconds::zero();
        }
        const auto declared = *runs_[index].declared;
        if( runs_[index].call_duration <= declared )
        {
            return std::chrono::nanoseconds::zero();
        }
        return runs_[index].call_duration - declared;
    }

    grab::NeutralizationOutcome
    Player::neutralization() const noexcept
    {
        return neutralization_;
    }

    const grab::Error*
    Player::failure() const noexcept
    {
        return failure_.has_value() ? &*failure_ : nullptr;
    }

    grab::Uuid
    Player::run_id() const noexcept
    {
        return run_id_;
    }

    const Sequence*
    Player::program() const noexcept
    {
        return program_;
    }

    const diag::Instrument&
    Player::instrument() const noexcept
    {
        return instrument_;
    }

    std::size_t
    Player::deepest_frontier() const noexcept
    {
        return deepest_frontier_;
    }

}    // namespace grab::kernel::sequence
