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
        note_unavailable( grab::CommandKind kind,
                          std::string_view  capability )
        {
            log::nominal(
                [kind, capability]( auto& event )
                {
                    event.tag( log::tags::sequence )
                        .value( "command", grab::command_name( kind ) )
                        .value( "missing_capability", capability )
                        .value( "error", "seat cannot run this command" );
                }
            );
            return grab::sequence::Status::Failure;
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
        void
        note_release( grab::CommandKind kind,
                      std::string_view  what,
                      bool              succeeded )
        {
            log::nominal(
                [kind, what, succeeded]( auto& event )
                {
                    event.tag( log::tags::sequence )
                        .value( "command", grab::command_name( kind ) )
                        .value( "released", what )
                        .value( "ok", succeeded );
                }
            );
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
