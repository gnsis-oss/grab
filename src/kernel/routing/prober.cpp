#include "kernel/routing/prober.hpp"
#include "kernel/support/environment.hpp"
#include "kernel/support/posix_open.h"

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace grab::core
{

    namespace
    {

        constexpr std::string_view uinputPath      = "/dev/uinput";
        constexpr std::string_view inputDevicePath = "/dev/input";
        constexpr std::string_view eventPrefix     = "event";
        constexpr char             envAssign       = '=';

        [[nodiscard]]
        std::optional<std::string>
        read_live_environment( std::string_view name )
        {
            const std::string prefix = std::string{ name } + envAssign;
            for( char* const* entry = ::environ; entry != nullptr && *entry != nullptr;
                 entry              = std::next( entry ) )
            {
                const std::string_view variable{ *entry };
                if( variable.starts_with( prefix ) )
                {
                    return std::string{ variable.substr( prefix.size() ) };
                }
            }

            return std::nullopt;
        }

    }    // namespace

    Environment
    probe_environment( const SystemFacts& facts )
    {
        Environment                      env;

        const std::optional<std::string> wayland = facts.get_env( "WAYLAND_DISPLAY" );
        const std::optional<std::string> x11     = facts.get_env( "DISPLAY" );
        if( wayland.has_value() && !wayland->empty() )
        {
            env.session          = SessionType::Wayland;
            env.xwayland_present = x11.has_value() && !x11->empty();
        }
        else if( x11.has_value() && !x11->empty() )
        {
            env.session = SessionType::X11;
        }
        else
        {
            env.session = SessionType::Headless;
        }

        env.desktop = facts.get_env( "XDG_CURRENT_DESKTOP" ).value_or( "" );

        const std::vector<std::string> devices = facts.list_input_devices();
        env.input_devices.reserve( devices.size() );
        for( const std::string& path : devices )
        {
            env.input_devices.push_back( InputDeviceAccess{
                .path     = path,
                .readable = facts.path_readable( path ),
            } );
        }
        env.uinput_writable = facts.path_writable( std::string{ uinputPath } );
        return env;
    }

    SystemFacts
    real_system_facts()
    {
        SystemFacts facts;
        facts.get_env = []( std::string_view name ) -> std::optional<std::string>
        {
            return read_live_environment( name );
        };
        // Probes must exercise real open(2) paths (spec §5) without creating or
        // truncating the target.
        facts.path_readable = []( const std::string& path )
        {
            return grab_open_read_probe( path.c_str() ) != 0;
        };
        facts.path_writable = []( const std::string& path )
        {
            return grab_open_write_probe( path.c_str() ) != 0;
        };
        facts.list_input_devices = []
        {
            std::vector<std::string> devices;
            std::error_code          ec;
            for( const std::filesystem::directory_entry& entry :
                 std::filesystem::directory_iterator(
                     std::filesystem::path{ inputDevicePath },
                     ec
                 ) )
            {
                const std::string name = entry.path().filename().string();
                if( name.starts_with( eventPrefix ) )
                {
                    devices.push_back( entry.path().string() );
                }
            }
            std::ranges::sort( devices );
            return devices;
        };
        return facts;
    }

}    // namespace grab::core
