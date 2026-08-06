#include "grab/command.hpp"
#include "grab/result.hpp"
#include "grab/sequence_types.hpp"
#include "kernel/sequence/sequence.hpp"
#include "kernel/sequence/sequence_ops.hpp"
#include "kernel/support/log.hpp"
#include "kernel/support/log_tags.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace grab::kernel::sequence
{

    namespace
    {

        // The ONLY duration a document declares is time.wait's. Every other op
        // is "unknown, so measure it" — the governing rule is that no duration
        // defaults to zero, so an unknown one is nullopt here and is counted by
        // unestimated_steps() rather than quietly folded in as 0.
        [[nodiscard]]
        std::optional<std::chrono::nanoseconds>
        declared_duration( const grab::sequence::Step& step ) noexcept
        {
            const auto* wait = std::get_if<grab::sequence::WaitCommand>( &step.command );
            if( wait == nullptr )
            {
                return std::nullopt;
            }
            return wait->duration;
        }

        // Grace is the interval between a step becoming READY and its enter, so
        // it lands on steps with at least one predecessor: a root starts
        // immediately, which is what "between" means. The mode is the sole
        // authority — extra_grace is ignored outside Precise rather than
        // rejected, so one document runs under all three modes unedited.
        [[nodiscard]]
        std::chrono::nanoseconds
        grace_before( const grab::sequence::Step&   step,
                      grab::sequence::PacingOptions pacing ) noexcept
        {
            if( step.after.empty() )
            {
                return std::chrono::nanoseconds::zero();
            }
            switch( pacing.mode )
            {
                case grab::sequence::PacingMode::Strict :
                    return std::chrono::nanoseconds::zero();
                case grab::sequence::PacingMode::Grace :
                    return pacing.grace;
                case grab::sequence::PacingMode::Precise :
                    return pacing.grace + step.extra_grace;
                case grab::sequence::PacingMode::Count :
                    break;
            }
            return std::chrono::nanoseconds::zero();
        }

        [[nodiscard]]
        grab::sequence::StepId
        shifted( grab::sequence::StepId id,
                 std::size_t            offset ) noexcept
        {
            const auto index = offset + static_cast<std::size_t>( id.index() );
            return grab::sequence::StepId{
                static_cast<grab::sequence::StepId::Half>( index ),
                grab::sequence::StepId::firstGeneration
            };
        }

        // Steps of `program` that nothing depends on. The host's successors of
        // the anchor are rewired onto these, which is what puts the injection
        // in series with the flow rather than beside it.
        [[nodiscard]]
        std::vector<grab::sequence::StepId>
        sinks_of( const Sequence& program,
                  std::size_t     offset )
        {
            std::vector<grab::sequence::StepId> found;
            for( const auto& step : program.steps() )
            {
                if( program.graph().out_edges( step.id ).empty() )
                {
                    found.push_back( shifted( step.id, offset ) );
                }
            }
            std::ranges::sort( found );
            return found;
        }

    }    // namespace

    grab::Result<Sequence>
    splice( const Sequence&        host,
            grab::sequence::StepId at,
            const Sequence&        insert )
    {
        if( host.find( at ) == nullptr )
        {
            std::string message{ "cannot splice at step index " };
            message.append( std::to_string( at.index() ) );
            message.append( ": the host sequence has no such step" );
            return grab::fail( grab::ErrorCode::InvalidArgument, message );
        }

        const auto hostSteps   = host.steps();
        const auto insertSteps = insert.steps();
        const auto offset      = hostSteps.size();

        if( offset + insertSteps.size() > grab::sequence::maxSteps )
        {
            std::string message{ "splicing " };
            message.append( std::to_string( insertSteps.size() ) );
            message.append( " steps into a sequence of " );
            message.append( std::to_string( offset ) );
            message.append( " would exceed the maximum of " );
            message.append( std::to_string( grab::sequence::maxSteps ) );
            return grab::fail( grab::ErrorCode::InvalidArgument, message );
        }

        for( const auto& step : insertSteps )
        {
            if( step.label.empty() )
            {
                continue;
            }
            if( host.resolve_label( step.label ).has_value() )
            {
                std::string message{ "spliced step label '" };
                message.append( step.label );
                message.append( "' already exists in the host sequence" );
                return grab::fail( grab::ErrorCode::InvalidArgument, message );
            }
        }

        std::vector<grab::sequence::Step> merged;
        merged.reserve( offset + insertSteps.size() );
        merged.assign( hostSteps.begin(), hostSteps.end() );

        if( !insertSteps.empty() )
        {
            const auto sinks = sinks_of( insert, offset );

            // Everything that waited on the anchor now waits on what was
            // injected after it. The anchor's own dependencies are untouched,
            // so no existing id and no unrelated branch moves.
            for( auto& step : merged )
            {
                auto found = std::ranges::find( step.after, at );
                if( found == step.after.end() )
                {
                    continue;
                }
                found = step.after.erase( found );
                step.after.insert( found, sinks.begin(), sinks.end() );
            }

            for( const auto& step : insertSteps )
            {
                grab::sequence::Step copy{ step };
                // build() re-stamps this positionally; setting it keeps the
                // remapped `after` entries below readable against it.
                copy.id = shifted( step.id, offset );
                if( copy.after.empty() )
                {
                    copy.after.push_back( at );
                }
                else
                {
                    for( auto& predecessor : copy.after )
                    {
                        predecessor = shifted( predecessor, offset );
                    }
                }
                merged.push_back( std::move( copy ) );
            }
        }

        auto built = Sequence::build( std::move( merged ),
                                      host.pacing(),
                                      std::string{ host.name() } );
        if( !built.has_value() )
        {
            return built;
        }

        log::nominal(
            [&host, &insert, at]( auto& event )
            {
                event.tag( log::tags::sequence )
                    .value( "spliced", insert.name() )
                    .value( "into", host.name() )
                    .value( "at", at.index() )
                    .value( "injected", insert.steps().size() );
            }
        );

        return built;
    }

    std::chrono::nanoseconds
    planned( const Sequence& program )
    {
        return planned( program, program.pacing() );
    }

    std::chrono::nanoseconds
    planned( const Sequence&               program,
             grab::sequence::PacingOptions pacing )
    {
        const auto                            steps = program.steps();
        std::vector<std::chrono::nanoseconds> finish( steps.size(),
                                                      std::chrono::nanoseconds::zero() );

        auto                                  total = std::chrono::nanoseconds::zero();

        // order() is topological, so every predecessor's finish is already
        // final by the time its successor is read.
        for( const auto id : program.order() )
        {
            const auto* step = program.find( id );
            if( step == nullptr )
            {
                continue;
            }

            auto ready = std::chrono::nanoseconds::zero();
            for( const auto predecessor : step->after )
            {
                const auto index = static_cast<std::size_t>( predecessor.index() );
                if( index < finish.size() )
                {
                    ready = std::max( ready, finish[index] );
                }
            }

            const auto own =
                declared_duration( *step ).value_or( std::chrono::nanoseconds::zero() );
            const auto done = ready + grace_before( *step, pacing ) + own;

            finish[static_cast<std::size_t>( id.index() )] = done;
            total                                          = std::max( total, done );
        }

        log::verbose(
            [&program, total, pacing]( auto& event )
            {
                event.tag( log::tags::sequence )
                    .value( "planned_ns", total.count() )
                    .value( "sequence", program.name() )
                    .value( "pacing", grab::sequence::pacing_mode_name( pacing.mode ) )
                    .value( "grace_ms", pacing.grace.count() );
            }
        );

        return total;
    }

    std::size_t
    unestimated_steps( const Sequence& program )
    {
        std::size_t unknown = 0U;
        for( const auto& step : program.steps() )
        {
            if( !declared_duration( step ).has_value() )
            {
                ++unknown;
            }
        }
        return unknown;
    }

    grab::Result<void>
    validate( const Sequence& program )
    {
        // Pass-through by decision, not by omission: resource and policy rules
        // land here later, and until they do a document that expresses
        // interleaved garbage input is run as written.
        ( void )program;
        return {};
    }

}    // namespace grab::kernel::sequence
