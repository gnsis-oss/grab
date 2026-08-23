#include "grab/command.hpp"
#include "grab/command_descriptor.hpp"
#include "grab/result.hpp"
#include "grab/sequence.hpp"
#include "grab/sequence_types.hpp"
#include "grab/session.hpp"
#include "grab/trace.hpp"
#include "kernel/sequence/drive.hpp"
#include "kernel/sequence/interpreter.hpp"
#include "kernel/sequence/player.hpp"
#include "kernel/sequence/runner.hpp"
#include "kernel/sequence/sequence.hpp"
#include "kernel/sequence/sequence_ops.hpp"
#include "sequence/session_seat.hpp"
#include "sequence/unwrap.hpp"

#include <expected>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>

namespace grab::sequence
{

    // The facade holds the kernel document behind a shared, immutable Impl:
    // copying a Sequence is copying a pointer, and nothing can mutate a
    // document two holders share.
    struct Sequence::Impl
    {
            grab::kernel::sequence::Sequence program;
    };

    namespace detail
    {

        // The one hole in the pimpl, for this library and the CLI only.
        struct Access
        {
                [[nodiscard]]
                static const grab::kernel::sequence::Sequence&
                program( const Sequence& sequence ) noexcept
                {
                    return sequence.impl_->program;
                }

                [[nodiscard]]
                static Sequence
                make( grab::kernel::sequence::Sequence program )
                {
                    return Sequence{ std::make_shared<const Sequence::Impl>(
                        Sequence::Impl{ std::move( program ) }
                    ) };
                }
        };

        const grab::kernel::sequence::Sequence&
        unwrap( const Sequence& sequence ) noexcept
        {
            return Access::program( sequence );
        }

        Sequence
        wrap( grab::kernel::sequence::Sequence program )
        {
            return Access::make( std::move( program ) );
        }

    }    // namespace detail

    namespace
    {

        // Every loader result passes the validate() seam exactly as the CLI
        // does, so "loaded" means the same thing on both surfaces.
        [[nodiscard]]
        grab::Result<Sequence>
        validated( grab::Result<grab::kernel::sequence::Sequence> program )
        {
            if( !program.has_value() )
            {
                return std::unexpected( std::move( program.error() ) );
            }
            auto valid = grab::kernel::sequence::validate( *program );
            if( !valid.has_value() )
            {
                return std::unexpected( std::move( valid.error() ) );
            }
            return detail::wrap( std::move( *program ) );
        }

        // Severity order for folding the unwind's outcome with the
        // process-exit backstop's: a failure to release anywhere is the
        // report, however well the other half went.
        [[nodiscard]]
        constexpr int
        severity( grab::NeutralizationOutcome outcome ) noexcept
        {
            switch( outcome )
            {
                case grab::NeutralizationOutcome::Failed :
                    return 3;
                case grab::NeutralizationOutcome::Released :
                    return 2;
                case grab::NeutralizationOutcome::NothingHeld :
                    return 1;
                case grab::NeutralizationOutcome::NotAttempted :
                    break;
            }
            return 0;
        }

        [[nodiscard]]
        constexpr grab::NeutralizationOutcome
        fold( grab::NeutralizationOutcome left,
              grab::NeutralizationOutcome right ) noexcept
        {
            return severity( left ) >= severity( right ) ? left : right;
        }

    }    // namespace

    Sequence::Sequence() :
        impl_( std::make_shared<const Impl>() )
    {
    }

    Sequence::~Sequence()                 = default;
    Sequence::Sequence( const Sequence& ) = default;
    Sequence&
    Sequence::operator=( const Sequence& )    = default;
    Sequence::Sequence( Sequence&& ) noexcept = default;
    Sequence&
    Sequence::operator=( Sequence&& ) noexcept = default;

    Sequence::Sequence( std::shared_ptr<const Impl> impl ) noexcept :
        impl_( std::move( impl ) )
    {
    }

    std::string_view
    Sequence::name() const noexcept
    {
        return impl_->program.name();
    }

    std::size_t
    Sequence::step_count() const noexcept
    {
        return impl_->program.steps().size();
    }

    PacingOptions
    Sequence::pacing() const noexcept
    {
        return impl_->program.pacing();
    }

    std::optional<StepId>
    Sequence::resolve_label( std::string_view label ) const noexcept
    {
        return impl_->program.resolve_label( label );
    }

    std::string_view
    Sequence::label_of( StepId id ) const noexcept
    {
        const auto* const step = impl_->program.find( id );
        return step == nullptr ? std::string_view{} : std::string_view{ step->label };
    }

    std::string_view
    Sequence::op_of( StepId id ) const noexcept
    {
        const auto* const step = impl_->program.find( id );
        return step == nullptr ? std::string_view{}
                               : grab::command_name( kind_of( step->command ) );
    }

    grab::Result<Sequence>
    load( std::string_view json )
    {
        return validated( grab::kernel::sequence::parse( json ) );
    }

    grab::Result<Sequence>
    load_file( const std::filesystem::path& path )
    {
        return validated( grab::kernel::sequence::load( path ) );
    }

    grab::Result<std::string>
    to_json( const Sequence& sequence )
    {
        return grab::kernel::sequence::to_json( detail::unwrap( sequence ) );
    }

    grab::Result<PlayReport>
    play( Session&        session,
          const Sequence& sequence,
          PlayOptions     options )
    {
        if( !session.is_open() )
        {
            return grab::fail( grab::ErrorCode::SessionClosed,
                               "play needs an open session" );
        }

        // The document under the caller's pacing overrides, exactly the way
        // `grab play --pacing`/`--grace-ms` apply theirs. Ids are positional,
        // so the rebuild preserves every StepId.
        const auto&         document = detail::unwrap( sequence );
        const PacingOptions pacing{
            .mode  = options.mode.value_or( document.pacing().mode ),
            .grace = options.grace.value_or( document.pacing().grace ),
        };
        auto program = grab::kernel::sequence::with_pacing( document, pacing );
        if( !program.has_value() )
        {
            return std::unexpected( std::move( program.error() ) );
        }

        // Input lands on the display the session was opened on — a seat that
        // silently connected elsewhere would click on a display the caller
        // never named. Overlay steps are bound to the session itself.
        const auto& display = session.display();
        auto seat = SessionSeat::open( display.has_value() ? display->c_str() : nullptr,
                                       std::string_view{} );
        if( !seat.has_value() )
        {
            return std::unexpected( std::move( seat.error() ) );
        }
        seat->bind_session( session );

        grab::kernel::sequence::SeatRunner<SessionSeat>       runner{ *seat };
        grab::kernel::sequence::OutstandingHolds<SessionSeat> holds{ runner };
        grab::kernel::sequence::Player                        player{ *program, runner };

        grab::kernel::sequence::DriveOptions                  drive_options;
        if( options.stop.stop_possible() )
        {
            drive_options.cancelled = [stop = options.stop]
            {
                return stop.stop_requested();
            };
            drive_options.cancel_reason = "the run was cancelled by its stop token";
        }
        auto       outcome  = grab::kernel::sequence::drive( player, drive_options );
        const auto released = holds.release();

        PlayReport report;
        report.state          = player.state();
        report.elapsed        = player.elapsed();
        report.run_id         = player.run_id();
        report.neutralization = fold( player.neutralization(), released );
        report.steps.reserve( program->steps().size() );
        for( const auto& step : program->steps() )
        {
            const auto timing = player.timing_of( step.id );
            report.steps.push_back( StepOutcome{
                .id            = step.id,
                .label         = step.label,
                .op            = grab::command_name( kind_of( step.command ) ),
                .status        = player.status_of( step.id ),
                .call_duration = timing.call_duration,
                .declared      = timing.declared,
                .overrun       = player.overrun_of( step.id ),
            } );
        }

        if( !outcome.has_value() )
        {
            report.failure = std::move( outcome.error() );
        }
        else if( player.failure() != nullptr )
        {
            report.failure = *player.failure();
        }
        else if( released == grab::NeutralizationOutcome::Failed )
        {
            auto stranded = grab::fail(
                grab::ErrorCode::ProviderFailed,
                "the document left a button, key or pointer capture down and "
                "it could not be released"
            );
            report.failure = std::move( stranded.error() );
        }
        return report;
    }

}    // namespace grab::sequence
