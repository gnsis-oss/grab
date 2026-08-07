// The seat-independent half of the execution triple. enter/tick/exit
// themselves are templated on the seat and therefore live in the header; what
// is here is everything that does not depend on it — the descriptor lookups,
// the argument checks, and the log records the diagnosis path needs.

#include "grab/command.hpp"
#include "grab/command_descriptor.hpp"
#include "grab/drag.hpp"
#include "grab/geometry/point.hpp"
#include "grab/result.hpp"
#include "grab/sequence_types.hpp"
#include "kernel/sequence/execute.hpp"
#include "kernel/support/log.hpp"
#include "kernel/support/log_tags.hpp"

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace grab::kernel::sequence
{

    grab::sequence::TimingClass
    timing_class_of( const grab::sequence::Command& command ) noexcept
    {
        return grab::timing_class_of( grab::sequence::kind_of( command ) );
    }

    bool
    is_blocking( const grab::sequence::Command& command ) noexcept
    {
        return grab::is_blocking_command( grab::sequence::kind_of( command ) );
    }

    namespace detail
    {

        bool
        in_seat_range( grab::geometry::Point point ) noexcept
        {
            return point.x >=
                   coordinateMinimum &&
                   point.x <=
                   coordinateMaximum &&
                   point.y >=
                   coordinateMinimum &&
                   point.y <= coordinateMaximum;
        }

        void
        note_failure( grab::CommandKind  kind,
                      std::string_view   stage,
                      const grab::Error& error )
        {
            log::nominal(
                [kind, stage, &error]( auto& event )
                {
                    event.tag( log::tags::sequence )
                        .value( "command", grab::command_name( kind ) )
                        .value( "stage", stage )
                        .value( "error", error.message );
                }
            );
        }

        grab::sequence::Status
        note_unavailable( grab::CommandKind kind )
        {
            log::nominal(
                [kind]( auto& event )
                {
                    event.tag( log::tags::sequence )
                        .value( "command", grab::command_name( kind ) )
                        // The CONCEPT, not a category: whoever has to fix the
                        // seat needs a name they can grep for in execute.hpp,
                        // and "overlay" is not one.
                        .value( "missing_concept", required_capability( kind ) )
                        .value( "error",
                                "seat does not satisfy the concept this "
                                "command needs" );
                }
            );
            return grab::sequence::Status::Failure;
        }

        void
        note_hold_taken( grab::CommandKind kind,
                         std::string_view  hold )
        {
            log::verbose(
                [kind, hold]( auto& event )
                {
                    event.tag( log::tags::sequence )
                        .value( "command", grab::command_name( kind ) )
                        .value( "hold", hold )
                        .value( "taken", true );
                }
            );
        }

        void
        note_deadline( grab::CommandKind         kind,
                       std::size_t               waypoints,
                       std::chrono::milliseconds dwell,
                       std::chrono::nanoseconds  span )
        {
            log::verbose(
                [kind, waypoints, dwell, span]( auto& event )
                {
                    event.tag( log::tags::sequence )
                        .value( "command", grab::command_name( kind ) )
                        .value( "deadline_in_us", as_micros( span ) )
                        .value( "waypoints", waypoints )
                        .value( "step_dwell_us", as_micros( dwell ) );
                }
            );
        }

        void
        note_deadline_met( grab::CommandKind        kind,
                           std::chrono::nanoseconds overshoot )
        {
            log::verbose(
                [kind, overshoot]( auto& event )
                {
                    event.tag( log::tags::sequence )
                        .value( "command", grab::command_name( kind ) )
                        .value( "deadline", "met" )
                        .value( "late_us", as_micros( overshoot ) );
                }
            );
        }

        // Built through fail() rather than by aggregate initialization:
        // -Wmissing-designated-field-initializers is live and grab::Error has
        // members carrying no default initializer, so a literal would have to
        // name every one of them at each site.
        grab::sequence::Status
        note_invalid( grab::CommandKind kind,
                      std::string_view  stage,
                      std::string       message )
        {
            const auto failure =
                grab::fail( grab::ErrorCode::InvalidArgument, std::move( message ) );
            note_failure( kind, stage, failure.error() );
            return grab::sequence::Status::Failure;
        }

        // Nominal rather than verbose: a run that put a button or a key back up
        // is the record that answers "did the interrupt strand anything", and
        // that question is asked exactly when logging is turned on after the
        // fact.
        //
        // `hold` and `reason` are the two halves of §6.1's distinction. An
        // implicit hold is released because the command that took it finished
        // with it; a document hold is released ONLY because the unwind proved
        // the step that was supposed to lift it will never run. Without both,
        // a chord that completed and a chord that was cut short produce the
        // same line — and that ambiguity has already hidden one real bug.
        void
        note_release( grab::CommandKind kind,
                      std::string_view  what,
                      std::string_view  hold,
                      std::string_view  reason,
                      bool              succeeded )
        {
            log::nominal(
                [kind, what, hold, reason, succeeded]( auto& event )
                {
                    event.tag( log::tags::sequence )
                        .value( "command", grab::command_name( kind ) )
                        .value( "released", what )
                        .value( "hold", hold )
                        .value( "reason", reason )
                        .value( "ok", succeeded );
                }
            );
        }

        grab::sequence::Status
        settle_overlay( grab::CommandKind         kind,
                        std::string_view          handle,
                        const grab::Result<void>& outcome )
        {
            if( !outcome )
            {
                note_failure( kind, "enter", outcome.error() );
                return grab::sequence::Status::Failure;
            }
            // Debug rather than verbose: enter() already records the command
            // and its status at verbose, so this adds only the handle — which
            // is what tells "it drew the shape I named" from "it drew a
            // shape", and is the first thing anyone asks of a scene that came
            // out wrong.
            log::debug(
                [kind, handle]( auto& event )
                {
                    event.tag( log::tags::sequence )
                        .value( "command", grab::command_name( kind ) )
                        .value( "handle", handle );
                }
            );
            return grab::sequence::Status::Success;
        }

        grab::sequence::Status
        note_missing_handle( grab::CommandKind kind )
        {
            return note_invalid( kind,
                                 "handle",
                                 "this overlay step needs the handle of a shape the "
                                 "document already named; only overlay.add may omit "
                                 "one, and an unhandled add is fire-and-forget" );
        }

        grab::Result<void>
        validate_options( const grab::input::DragOptions& options )
        {
            if( options.interpolation_steps <
                grab::input::DragOptions::minimumInterpolationSteps ||
                options.interpolation_steps >
                grab::input::DragOptions::maximumInterpolationSteps )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "interpolation-step count is out of range" );
            }
            if( options.step_dwell < std::chrono::milliseconds::zero() )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "step dwell must not be negative" );
            }
            return {};
        }

        grab::Result<void>
        validate_reachable( const std::vector<grab::geometry::Point>& points )
        {
            for( const auto point : points )
            {
                if( !in_seat_range( point ) )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "pointer coordinate is outside int16 range" );
                }
            }
            return {};
        }

    }    // namespace detail

}    // namespace grab::kernel::sequence
