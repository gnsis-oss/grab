#include "grab/command.hpp"
#include "grab/command_descriptor.hpp"
#include "grab/ids.hpp"
#include "grab/result.hpp"
#include "grab/sequence_types.hpp"
#include "grab/trace.hpp"
#include "kernel/identity/id_factory.hpp"
#include "kernel/sequence/player.hpp"
#include "kernel/sequence/sequence.hpp"
#include "kernel/support/log.hpp"
#include "kernel/support/log_tags.hpp"

#include <algorithm>
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
    Player::drop_from_frontier( grab::sequence::StepId id )
    {
        const auto found = std::ranges::find( frontier_, id );
        if( found != frontier_.end() )
        {
            frontier_.erase( found );
        }
    }

    void
    Player::neutralize( grab::sequence::StepId id,
                        TimePoint              now )
    {
        const auto index = index_of( id );
        if( index >= runs_.size() )
        {
            return;
        }
        auto& run = runs_[index];
        if( !run.entered || run.exited )
        {
            return;
        }
        const auto* const step = program_->find( id );
        if( step == nullptr )
        {
            return;
        }

        // A Blocking body runs on a worker, so join it before exit(). It costs
        // the rest of that step; abandoning the worker would race exit()
        // against a capture still writing its buffer.
        if( grab::is_blocking_command( grab::sequence::kind_of( step->command ) ) )
        {
            runner_->join( *step );
        }
        const auto outcome = runner_->exit( *step, now );
        run.exited         = true;

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
            [&step, outcome]( auto& event )
            {
                event.tag( log::tags::player )
                    .value( "neutralized", step->id.index() )
                    .value( "outcome", outcome );
            }
        );
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

    void
    Player::reap_holds( grab::sequence::StepId id )
    {
        const auto* const step = program_->find( id );
        if( step == nullptr )
        {
            return;
        }
        const auto outcome = runner_->release_holds( *step );
        record_neutralization( outcome );

        log::verbose(
            [&step, outcome]( auto& event )
            {
                event.tag( log::tags::player )
                    .value( "reaped", step->id.index() )
                    .value( "outcome", outcome );
            }
        );
    }

    void
    Player::admit_successors( grab::sequence::StepId id,
                              TimePoint              now )
    {
        ( void )now;
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
            bool      ready = true;
            for( const auto predecessor : step->after )
            {
                if( !is_terminal( predecessor ) )
                {
                    ready = false;
                    break;
                }
                eligible =
                    std::max( eligible, runs_[index_of( predecessor )].finished_at );
            }
            if( !ready )
            {
                continue;
            }

            status_[successor_index] = grab::sequence::StepStatus::Ready;
            runs_[successor_index].ready_at =
                add_grace( eligible, grace_before( *step, pacing ) );
            frontier_.push_back( successor );
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
            ( void )runner_->exit( *step, now );
            run.exited = true;
        }
        status_[index] = grab::sequence::StepStatus::Succeeded;
        last_finish_   = std::max( last_finish_, now );
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
        neutralize( id, now );
        status_[index] = grab::sequence::StepStatus::Failed;
        last_finish_   = std::max( last_finish_, now );
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
                        state_ = grab::sequence::PlayState::Interrupted;
                        return;
                    }
                    auto jumped = jump_to( *target );
                    if( !jumped.has_value() )
                    {
                        failure_ = jumped.error();
                        unwind( now );
                        state_ = grab::sequence::PlayState::Interrupted;
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
        state_ = grab::sequence::PlayState::Interrupted;
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

        auto code = runner_->last_error( *step );
        if( may_retry( *step, code ) && run.retries < maxRetriesPerStep )
        {
            run.retries += 1U;
            neutralize( id, now );
            run.exited       = false;
            run.entered_at   = now;
            const auto again = runner_->enter( *step, now );
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
            if( runs_[index].exited )
            {
                // succeed() exits a cleanly-completed step, so neutralize()
                // can never revisit it — and an EXPLICIT, document-owned hold
                // is still down, waiting for a later step this unwind has just
                // cancelled. release_holds is the seam that lifts it, and it
                // is deliberately not a second exit(): that contract is
                // exactly-once, and calling it twice would double-release the
                // implicit case.
                reap_holds( id );
                continue;
            }
            runs_[index].call_duration = now - runs_[index].entered_at;
            neutralize( id, now );
            if( !terminal_status( status_[index] ) )
            {
                status_[index]           = grab::sequence::StepStatus::Skipped;
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
                status_[index]           = grab::sequence::StepStatus::Skipped;
                runs_[index].finished_at = now;
            }
        }
        frontier_.clear();
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
                neutralize( ancestor, now );
            }
            status_[index]           = grab::sequence::StepStatus::Skipped;
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
                neutralize( member, now );
                status_[index]           = grab::sequence::StepStatus::Skipped;
                runs_[index].finished_at = now;
                continue;
            }
            status_[index] = grab::sequence::StepStatus::Pending;
        }

        frontier_.clear();
        frontier_.push_back( target );

        const auto target_index = index_of( target );
        if( status_[target_index] != grab::sequence::StepStatus::Running )
        {
            status_[target_index] = grab::sequence::StepStatus::Ready;
            runs_[target_index].ready_at =
                add_grace( now, grace_before( *step, program_->pacing() ) );
        }

        log::nominal(
            [&step]( auto& event )
            {
                event.tag( log::tags::player ).value( "goto", step->id.index() );
            }
        );
        return {};
    }

    grab::Result<void>
    Player::play()
    {
        if( state_ == grab::sequence::PlayState::Paused )
        {
            state_ = grab::sequence::PlayState::Playing;
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

        state_ = grab::sequence::PlayState::Playing;
        for( const auto& step : program_->steps() )
        {
            if( !step.after.empty() )
            {
                continue;
            }
            const auto index = index_of( step.id );
            status_[index]   = grab::sequence::StepStatus::Ready;
            // Roots start immediately: grace is the gap between steps.
            runs_[index].ready_at = last_now_;
            frontier_.push_back( step.id );
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
        state_ = grab::sequence::PlayState::Paused;
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
        state_ = grab::sequence::PlayState::Interrupted;
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
                neutralize( id, now );
            }
            status_[index]           = grab::sequence::StepStatus::Skipped;
            runs_[index].finished_at = now;
        }
        for( const auto id : scratch_ )
        {
            admit_successors( id, now );
        }
        if( state_ == grab::sequence::PlayState::Playing && frontier_.empty() )
        {
            state_ = grab::sequence::PlayState::Done;
        }
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

        const bool        failed_before = failure_.has_value();

        // Bounded because every iteration that makes progress moves at least
        // one step through enter or through completion, and each step does
        // both at most once per retry.
        const std::size_t limit = ( 2U * status_.size() ) + 2U;
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
                const auto status = runner_->tick( *step, now );
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

            // Paused admits NO new steps, but the running ones above still run
            // to completion: pausing mid-drag would strand a held button.
            if( state_ == grab::sequence::PlayState::Playing )
            {
                scratch_.assign( frontier_.begin(), frontier_.end() );
                for( const auto id : scratch_ )
                {
                    const auto index = index_of( id );
                    if( index >=
                        status_.size() ||
                        status_[index] !=
                        grab::sequence::StepStatus::Ready ||
                        runs_[index].ready_at > now )
                    {
                        continue;
                    }
                    const auto* const step = program_->find( id );
                    if( step == nullptr )
                    {
                        continue;
                    }

                    status_[index]          = grab::sequence::StepStatus::Running;
                    runs_[index].entered    = true;
                    runs_[index].exited     = false;
                    runs_[index].entered_at = now;
                    runs_[index].declared   = runner_->declared_duration( *step );
                    entry_order_.push_back( id );
                    if( !anything_entered_ )
                    {
                        anything_entered_ = true;
                        first_entry_      = now;
                    }
                    progressed        = true;

                    const auto status = runner_->enter( *step, now );
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

        // A run reaches Done when the FRONTIER EMPTIES, not when every step is
        // terminal: goto leaves unrelated branches Pending and does not wait
        // on them.
        if( state_ == grab::sequence::PlayState::Playing && frontier_.empty() )
        {
            state_ = grab::sequence::PlayState::Done;
            log::nominal(
                [this]( auto& event )
                {
                    event.tag( log::tags::player )
                        .value( "done", program_->name() )
                        .value( "elapsed_us",
                                std::chrono::duration_cast<std::chrono::microseconds>(
                                    elapsed()
                                )
                                    .count() );
                }
            );
        }

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

}    // namespace grab::kernel::sequence
