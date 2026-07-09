#include "core/doctor.hpp"
#include "core/environment.hpp"
#include "core/json.hpp"
#include "core/log.hpp"
#include "core/provider.hpp"
#include "core/registry.hpp"
#include "core/resolver.hpp"
#include "grab/capability.hpp"
#include "grab/enum_table.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace grab::core
{

    namespace
    {

        inline constexpr auto kSessionNames = EnumTable{
            std::to_array( {
                enum_entry( SessionType::x11, "x11" ),
                enum_entry( SessionType::wayland, "wayland" ),
                enum_entry( SessionType::headless, "headless" ),
                enum_entry( SessionType::unknown, "unknown" ),
            } ),
        };
        static_assert( enum_table_has_count( kSessionNames,
                                             SessionType::count ) );

        struct CapabilityNameLess
        {
                [[nodiscard]]
                bool
                operator()( Capability lhs,
                            Capability rhs ) const noexcept
                {
                    return capability_name( lhs ) < capability_name( rhs );
                }
        };

        [[nodiscard]]
        std::string_view
        session_name( SessionType session ) noexcept
        {
            return kSessionNames.text_of( session, "unknown" );
        }

    }    // namespace

    DoctorReport
    run_doctor( const Registry&    registry,
                const Environment& env )
    {
        DoctorReport report;
        report.environment   = env;

        const auto providers = registry.all();
        log::nominal(
            [&providers]( auto& event )
            {
                event.tag( "doctor.probe" )
                    .value( "phase", "start" )
                    .value( "providers", providers.size() );
            }
        );

        std::set<Capability, CapabilityNameLess> capability_ids;
        for( const auto* provider : providers )
        {
            for( const auto& id : provider->info().capabilities )
            {
                capability_ids.insert( id );
            }
        }

        const Resolver       resolver( registry );
        const ResolveOptions default_options;
        for( const auto& id : capability_ids )
        {
            CapabilityReport entry;
            entry.id              = capability_name( id );
            const auto resolution = resolver.resolve(
                CapabilityRequest{
                    .capability   = id,
                    .target_class = "",
                    .target_key   = "",
                    .options      = default_options,
                },
                env
            );
            if( resolution.has_value() )
            {
                entry.provider = resolution->chain.front()->info().name;
                entry.state    = resolution->best.state;
                entry.reason   = resolution->best.reason;
            }
            else
            {
                entry.state = AvailabilityState::unavailable;
                if( !resolution.error().attempts.empty() )
                {
                    entry.provider = resolution.error().attempts.front().provider;
                    entry.reason   = resolution.error().attempts.front().reason;
                }
                entry.remediation = resolution.error().message;
            }
            report.capabilities.push_back( std::move( entry ) );
        }
        log::nominal(
            [&report]( auto& event )
            {
                event.tag( "doctor.probe" )
                    .value( "phase", "end" )
                    .value( "capabilities", report.capabilities.size() );
            }
        );
        return report;
    }

    std::string
    to_json( const DoctorReport& report )
    {
        json::Writer writer;
        writer.begin_object();

        writer.field_object_start( "environment" );
        writer.field( "session", session_name( report.environment.session ) );
        writer.field( "xwayland", report.environment.xwayland_present );
        writer.field( "desktop", report.environment.desktop );
        writer.field( "generation", report.environment.generation );
        writer.field( "uinput_writable", report.environment.uinput_writable );
        writer.begin_array( "input_devices" );
        for( const auto& device : report.environment.input_devices )
        {
            writer.begin_object_in_array();
            writer.field( "path", device.path );
            writer.field( "readable", device.readable );
            writer.end_object();
        }
        writer.end_array();
        writer.end_object();

        writer.begin_array( "capabilities" );
        for( const auto& entry : report.capabilities )
        {
            writer.begin_object_in_array();
            writer.field( "id", entry.id );
            writer.field( "status", state_name( entry.state ) );
            writer.field( "provider", entry.provider );
            writer.field( "degradation_reason", entry.reason );
            writer.field( "remediation", entry.remediation );
            writer.end_object();
        }
        writer.end_array();

        writer.end_object();
        return std::move( writer ).take();
    }

    std::string
    to_text( const DoctorReport& report )
    {
        std::string out;
        out += "session: ";
        out += session_name( report.environment.session );
        out += report.environment.xwayland_present ? " (+xwayland)\n" : "\n";
        if( report.capabilities.empty() )
        {
            out += "no capability providers registered\n";
        }
        for( const auto& entry : report.capabilities )
        {
            out += entry.id;
            out += ": ";
            out += state_name( entry.state );
            if( !entry.provider.empty() )
            {
                out += " via ";
                out += entry.provider;
            }
            if( !entry.reason.empty() )
            {
                out += " (";
                out += entry.reason;
                out += ')';
            }
            out += '\n';
            if( !entry.remediation.empty() )
            {
                out += "  fix: ";
                out += entry.remediation;
                out += '\n';
            }
        }
        return out;
    }

    int
    doctor_exit_code( const DoctorReport& report )
    {
        const bool all_available =
            std::ranges::all_of( report.capabilities,
                                 []( const CapabilityReport& entry )
                                 {
                                     return entry.state == AvailabilityState::available;
                                 } );
        return all_available ? 0 : 1;
    }

}    // namespace grab::core
