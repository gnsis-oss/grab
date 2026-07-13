#include "core/log.hpp"
#include "grab/pid.hpp"
#include "grab/result.hpp"
#include "grab/workspace.hpp"
#include "session/manager.hpp"
#include "session/provider.hpp"
#include "session/record.hpp"
#include "session/registry.hpp"
#include "session/state_machine.hpp"

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace grab::session
{
    namespace
    {

        [[nodiscard]]
        SessionRecord
        starting_record( const WorkspaceDesc&   desc,
                         const SessionProvider& provider )
        {
            return SessionRecord{
                .name              = desc.name,
                .provider          = provider.info().name,
                .endpoint          = {},
                .control_socket    = {},
                .mode              = desc.mode,
                .geometry          = desc.geometry,
                .state             = WorkspaceState::Starting,
                .supervisor_pid    = grab::Pid{},
                .created_monotonic = 0U,
            };
        }

        [[nodiscard]]
        SessionRuntime
        runtime_from_record( const SessionRecord& record )
        {
            return SessionRuntime{
                .endpoint       = record.endpoint,
                .control_socket = record.control_socket,
                .supervisor_pid = record.supervisor_pid,
            };
        }

        void
        log_transition( std::string_view name,
                        WorkspaceState   state )
        {
            grab::log::nominal(
                [&]( grab::log::Event& event )
                {
                    event.tag( "session.transition" )
                        .value( "name", name )
                        .value( "state", grab::state_name( state ) );
                }
            );
        }

        void
        log_rollback( std::string_view name,
                      std::string_view reason )
        {
            grab::log::nominal(
                [&]( grab::log::Event& event )
                {
                    event.tag( "session.rollback" )
                        .value( "name", name )
                        .value( "reason", reason );
                }
            );
        }

        void
        remove_after_failed_start( SessionRegistry& registry,
                                   std::string_view name )
        {
            const auto removed = registry.remove( name );
            if( removed.has_value() )
            {
                return;
            }
            grab::log::nominal(
                [&]( grab::log::Event& event )
                {
                    event.tag( "session.rollback_failed" )
                        .value( "name", name )
                        .value( "reason", removed.error().message );
                }
            );
        }

        [[nodiscard]]
        grab::Result<void>
        ensure_transition( WorkspaceState from,
                           WorkspaceState to )
        {
            if( is_valid_transition( from, to ) )
            {
                return {};
            }
            return grab::fail( ErrorCode::InternalFault,
                               std::string{ "invalid session transition: " } +
                                   std::string{ grab::state_name( from ) } +
                                   " -> " +
                                   std::string{ grab::state_name( to ) } );
        }

    }    // namespace

    SessionManager::SessionManager( SessionRegistry&       registry,
                                    const SessionProvider& provider ) noexcept :
        registry( registry ),
        provider( provider )
    {
    }

    grab::Result<SessionRecord>
    SessionManager::start( const WorkspaceDesc& desc )
    {
        auto record = starting_record( desc, provider );
        log_transition( record.name, record.state );

        const auto created = registry.create( record );
        if( !created.has_value() )
        {
            return std::unexpected( created.error() );
        }

        const auto runtime = provider.create( desc );
        if( !runtime.has_value() )
        {
            remove_after_failed_start( registry, record.name );
            log_rollback( record.name, runtime.error().message );
            return std::unexpected( runtime.error() );
        }

        auto transition = ensure_transition( record.state, WorkspaceState::Ready );
        if( !transition.has_value() )
        {
            remove_after_failed_start( registry, record.name );
            return std::unexpected( transition.error() );
        }

        record.endpoint       = runtime->endpoint;
        record.control_socket = runtime->control_socket;
        record.supervisor_pid = runtime->supervisor_pid;
        record.state          = WorkspaceState::Ready;
        log_transition( record.name, record.state );

        const auto written = registry.write( record );
        if( !written.has_value() )
        {
            remove_after_failed_start( registry, record.name );
            return std::unexpected( written.error() );
        }

        return record;
    }

    grab::Result<void>
    SessionManager::stop( std::string_view name )
    {
        auto record = registry.read( name );
        if( !record.has_value() )
        {
            return std::unexpected( record.error() );
        }

        auto transition = ensure_transition( record->state, WorkspaceState::Draining );
        if( !transition.has_value() )
        {
            return std::unexpected( transition.error() );
        }

        record->state = WorkspaceState::Draining;
        log_transition( record->name, record->state );
        const auto written = registry.write( *record );
        if( !written.has_value() )
        {
            return std::unexpected( written.error() );
        }

        const auto runtime   = runtime_from_record( *record );
        const auto destroyed = provider.destroy( runtime );
        if( !destroyed.has_value() )
        {
            return std::unexpected( destroyed.error() );
        }

        transition = ensure_transition( record->state, WorkspaceState::Stopped );
        if( !transition.has_value() )
        {
            return std::unexpected( transition.error() );
        }

        log_transition( record->name, WorkspaceState::Stopped );
        return registry.remove( record->name );
    }

    grab::Result<SessionRecord>
    SessionManager::get( std::string_view name )
    {
        return registry.read( name );
    }

    std::vector<SessionRecord>
    SessionManager::list()
    {
        return registry.list();
    }

}    // namespace grab::session
