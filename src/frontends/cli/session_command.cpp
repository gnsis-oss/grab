#include "frontends/cli/capture_command.hpp"
#include "frontends/cli/session_command.hpp"
#include "grab/capability.hpp"
#include "grab/result.hpp"
#include "grab/workspace.hpp"
#include "kernel/routing/prober.hpp"
#include "kernel/support/environment.hpp"
#include "session/builtin_session_providers.hpp"
#include "session/manager.hpp"
#include "session/record.hpp"
#include "session/registry.hpp"
#include "session/selection.hpp"

#include <cstddef>
#include <cstdio>
#include <iterator>
#include <span>
#include <string>
#include <string_view>

namespace grab::cli
{

    namespace
    {

        constexpr int              success_exit_code     = 0;
        constexpr int              error_exit_code       = 1;
        constexpr std::size_t      name_position         = 1U;
        constexpr std::size_t      first_option_position = 2U;
        constexpr std::string_view start_command         = "start";
        constexpr std::string_view stop_command          = "stop";
        constexpr std::string_view list_command          = "list";
        constexpr std::string_view doctor_command        = "doctor";
        constexpr std::string_view mode_flag             = "--mode";
        constexpr std::string_view geometry_flag         = "--geometry";
        constexpr std::string_view app_flag              = "--app";
        constexpr std::string_view flag_prefix           = "--";

        [[nodiscard]]
        bool
        is_flag_like( std::string_view value ) noexcept
        {
            return value.starts_with( flag_prefix );
        }

        [[nodiscard]]
        grab::Result<WorkspaceGeometry>
        parse_session_geometry( std::string_view input )
        {
            const std::size_t dimension = input.find( detail::dimension_marker );
            if( dimension == std::string_view::npos )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "session geometry must match WxH" );
            }

            auto width  = detail::parse_nonzero_u16( input.substr( 0U, dimension ) );
            auto height = detail::parse_nonzero_u16( input.substr( dimension + 1U ) );
            if( !width.has_value() )
            {
                return grab::fail( width.error().code, width.error().message );
            }
            if( !height.has_value() )
            {
                return grab::fail( height.error().code, height.error().message );
            }

            return WorkspaceGeometry{
                .width  = *width,
                .height = *height,
            };
        }

        void
        print_text( std::FILE*       stream,
                    std::string_view text )
        {
            ( void )std::fwrite( text.data(), sizeof( char ), text.size(), stream );
        }

        void
        print_error( std::string_view message )
        {
            print_text( stderr, "grab: " );
            print_text( stderr, message );
            ( void )std::fputc( '\n', stderr );
        }

        void
        print_record( const session::SessionRecord& record )
        {
            print_text( stdout, record.name );
            ( void )std::fputc( ' ', stdout );
            print_text( stdout, grab::state_name( record.state ) );
            ( void )std::fputc( ' ', stdout );
            print_text( stdout, record.endpoint );
            ( void )std::fputc( '\n', stdout );
        }

        [[nodiscard]]
        grab::Result<std::string_view>
        parse_single_name( std::span<const std::string_view> args,
                           std::string_view                  command )
        {
            if( args.size() != first_option_position || args.front() != command )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   std::string{ "usage: grab session " } +
                                       std::string{ command } +
                                       " <name>" );
            }
            const std::string_view name = args.subspan( name_position ).front();
            if( name.empty() || is_flag_like( name ) )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "session name is required" );
            }
            return name;
        }

        [[nodiscard]]
        core::Environment
        probe_cli_environment()
        {
            const auto facts = core::real_system_facts();
            return core::probe_environment( facts );
        }

        [[nodiscard]]
        grab::Result<session::SessionRegistry>
        make_registry()
        {
            auto root = session::SessionRegistry::default_root();
            if( !root.has_value() )
            {
                return grab::fail( root.error().code, root.error().message );
            }
            return session::SessionRegistry{ *root };
        }

        int
        start_session( std::span<const std::string_view> args )
        {
            auto desc = parse_session_start_args( args );
            if( !desc.has_value() )
            {
                print_error( desc.error().message );
                return error_exit_code;
            }

            auto registry = make_registry();
            if( !registry.has_value() )
            {
                print_error( registry.error().message );
                return error_exit_code;
            }

            const auto providers = session::builtin_session_providers();
            const auto env       = probe_cli_environment();
            const auto chosen =
                session::select_session_provider( providers, env, desc->mode );
            if( !chosen.has_value() )
            {
                print_error( chosen.error().message );
                return error_exit_code;
            }

            session::SessionManager manager{ *registry, **chosen };
            auto                    record = manager.start( *desc );
            if( !record.has_value() )
            {
                print_error( record.error().message );
                return error_exit_code;
            }

            print_record( *record );
            return success_exit_code;
        }

        int
        stop_session( std::span<const std::string_view> args )
        {
            auto name = parse_single_name( args, stop_command );
            if( !name.has_value() )
            {
                print_error( name.error().message );
                return error_exit_code;
            }

            auto registry = make_registry();
            if( !registry.has_value() )
            {
                print_error( registry.error().message );
                return error_exit_code;
            }

            auto record = registry->read( *name );
            if( !record.has_value() )
            {
                print_error( record.error().message );
                return error_exit_code;
            }

            const auto providers = session::builtin_session_providers();
            const auto env       = probe_cli_environment();
            const auto chosen =
                session::select_session_provider( providers, env, record->mode );
            if( !chosen.has_value() )
            {
                print_error( chosen.error().message );
                return error_exit_code;
            }

            session::SessionManager manager{ *registry, **chosen };
            auto                    stopped = manager.stop( *name );
            if( !stopped.has_value() )
            {
                print_error( stopped.error().message );
                return error_exit_code;
            }

            return success_exit_code;
        }

        int
        list_sessions( std::span<const std::string_view> args )
        {
            if( args.size() != name_position || args.front() != list_command )
            {
                print_error( "usage: grab session list" );
                return error_exit_code;
            }

            auto registry = make_registry();
            if( !registry.has_value() )
            {
                print_error( registry.error().message );
                return error_exit_code;
            }

            const auto records = registry->list();
            for( const auto& record : records )
            {
                print_record( record );
            }

            return success_exit_code;
        }

        void
        print_mode_report( const session::SessionModeReport& report )
        {
            print_text( stdout, mode_name( report.mode ) );
            ( void )std::fputc( ' ', stdout );
            print_text( stdout, report.provider );
            ( void )std::fputc( ' ', stdout );
            print_text( stdout, state_name( report.state ) );
            if( !report.reason.empty() )
            {
                ( void )std::fputc( ' ', stdout );
                print_text( stdout, report.reason );
            }
            ( void )std::fputc( '\n', stdout );
        }

        int
        doctor_session( std::span<const std::string_view> args )
        {
            if( args.size() != name_position || args.front() != doctor_command )
            {
                print_error( "usage: grab session doctor" );
                return error_exit_code;
            }

            const auto providers = session::builtin_session_providers();
            const auto env       = probe_cli_environment();
            const auto report = session::session_availability_report( providers, env );
            for( const auto& row : report )
            {
                print_mode_report( row );
            }
            return success_exit_code;
        }

    }    // namespace

    grab::Result<grab::WorkspaceDesc>
    parse_session_start_args( std::span<const std::string_view> args )
    {
        if( args.size() <= name_position || args.front() != start_command )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "usage: grab session start <name> [options]" );
        }
        const std::string_view name = args.subspan( name_position ).front();
        if( name.empty() || is_flag_like( name ) )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "session name is required" );
        }

        WorkspaceDesc desc;
        desc.name    = std::string{ name };
        desc.mode    = WorkspaceMode::Offscreen;

        auto current = std::next( args.begin(),
                                  static_cast<std::ptrdiff_t>( first_option_position ) );
        while( current != args.end() )
        {
            const std::string_view flag           = *current;
            const auto             value_position = std::next( current );
            if( value_position == args.end() )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   std::string{ flag } + " requires a value" );
            }

            const std::string_view value = *value_position;
            if( flag == mode_flag )
            {
                auto mode = mode_from_string( value );
                if( !mode.has_value() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "unknown session mode: " + std::string{ value } );
                }
                desc.mode = *mode;
            }
            else if( flag == geometry_flag )
            {
                auto geometry = parse_session_geometry( value );
                if( !geometry.has_value() )
                {
                    return grab::fail( geometry.error().code, geometry.error().message );
                }
                desc.geometry = *geometry;
            }
            else if( flag == app_flag )
            {
                desc.app_command = value;
            }
            else
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "unknown session argument: " + std::string{ flag } );
            }

            current = std::next( value_position );
        }

        return desc;
    }

    bool
    is_session_subcommand( std::span<const std::string_view> args ) noexcept
    {
        if( args.empty() )
        {
            return false;
        }

        const std::string_view command = args.front();
        if( command == start_command || command == stop_command )
        {
            return true;
        }
        return command == list_command || command == doctor_command;
    }

    int
    run_session_command( std::span<const std::string_view> args )
    {
        if( args.empty() )
        {
            print_error( "usage: grab session <start|stop|list|doctor> [args]" );
            return error_exit_code;
        }

        const std::string_view command = args.front();
        if( !is_session_subcommand( args ) )
        {
            print_error( "unknown session command: " + std::string{ command } );
            return error_exit_code;
        }

        if( command == start_command )
        {
            return start_session( args );
        }
        if( command == stop_command )
        {
            return stop_session( args );
        }
        if( command == list_command )
        {
            return list_sessions( args );
        }
        return doctor_session( args );
    }

}    // namespace grab::cli
