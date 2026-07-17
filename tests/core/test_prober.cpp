#include "kernel/routing/prober.hpp"
#include "kernel/support/environment.hpp"

#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

    constexpr std::string_view displayName              = "DISPLAY";
    constexpr std::string_view displayValue             = ":0";
    constexpr std::string_view sessionTypeName          = "XDG_SESSION_TYPE";
    constexpr std::string_view x11SessionTypeValue      = "x11";
    constexpr std::string_view waylandDisplayName       = "WAYLAND_DISPLAY";
    constexpr std::string_view waylandDisplayValue      = "wayland-0";
    constexpr std::string_view currentDesktopName       = "XDG_CURRENT_DESKTOP";
    constexpr std::string_view kdeDesktopValue          = "KDE";
    constexpr std::string_view uinputPath               = "/dev/uinput";
    constexpr std::string_view firstInputDevicePath     = "/dev/input/event0";
    constexpr std::string_view secondInputDevicePath    = "/dev/input/event1";
    constexpr auto             expectedInputDeviceCount = 2U;
    constexpr auto             firstInputDeviceIndex    = 0U;
    constexpr auto             secondInputDeviceIndex   = 1U;

    using EnvVars = std::vector<std::pair<std::string, std::string>>;

    std::pair<std::string,
              std::string>
    env_var( std::string_view name,
             std::string_view value )
    {
        return { std::string{ name }, std::string{ value } };
    }

    grab::core::SystemFacts
    facts_with_env( EnvVars env )
    {
        grab::core::SystemFacts facts;
        facts.get_env = [env = std::move( env )](
                            std::string_view name
                        ) -> std::optional<std::string>
        {
            for( const auto& [key, value] : env )
            {
                if( key == name )
                {
                    return value;
                }
            }
            return std::nullopt;
        };
        facts.path_readable = []( const std::string& )
        {
            return false;
        };
        facts.path_writable = []( const std::string& )
        {
            return false;
        };
        facts.list_input_devices = []
        {
            return std::vector<std::string>{};
        };
        return facts;
    }

}    // namespace

TEST( Prober,
      DetectsPureX11 )
{
    const grab::core::Environment env = grab::core::probe_environment( facts_with_env( {
        env_var( displayName, displayValue ),
        env_var( sessionTypeName, x11SessionTypeValue ),
    } ) );
    EXPECT_EQ( env.session, grab::core::SessionType::X11 );
    EXPECT_FALSE( env.xwayland_present );
}

TEST( Prober,
      DetectsWaylandWithXwayland )
{
    const grab::core::Environment env = grab::core::probe_environment( facts_with_env( {
        env_var( waylandDisplayName, waylandDisplayValue ),
        env_var( displayName, displayValue ),
        env_var( currentDesktopName, kdeDesktopValue ),
    } ) );
    EXPECT_EQ( env.session, grab::core::SessionType::Wayland );
    EXPECT_TRUE( env.xwayland_present );
    EXPECT_EQ( env.desktop, kdeDesktopValue );
}

TEST( Prober,
      DetectsPureWaylandWithoutXwayland )
{
    const grab::core::Environment env = grab::core::probe_environment( facts_with_env( {
        env_var( waylandDisplayName, waylandDisplayValue ),
    } ) );
    EXPECT_EQ( env.session, grab::core::SessionType::Wayland );
    EXPECT_FALSE( env.xwayland_present );
}

TEST( Prober,
      DetectsHeadless )
{
    const grab::core::Environment env =
        grab::core::probe_environment( facts_with_env( {} ) );
    EXPECT_EQ( env.session, grab::core::SessionType::Headless );
}

TEST( Prober,
      ProbesUinputWriteAccess )
{
    grab::core::SystemFacts writable =
        facts_with_env( { env_var( displayName, displayValue ) } );
    writable.path_writable = []( const std::string& path )
    {
        return path == std::string{ uinputPath };
    };

    const grab::core::Environment writable_env =
        grab::core::probe_environment( writable );
    EXPECT_TRUE( writable_env.uinput_writable );

    grab::core::SystemFacts unwritable =
        facts_with_env( { env_var( displayName, displayValue ) } );
    unwritable.path_writable = []( const std::string& )
    {
        return false;
    };

    const grab::core::Environment unwritable_env =
        grab::core::probe_environment( unwritable );
    EXPECT_FALSE( unwritable_env.uinput_writable );
}

TEST( Prober,
      ProbesInputDeviceAccessPerDevice )
{
    grab::core::SystemFacts facts =
        facts_with_env( { env_var( displayName, displayValue ) } );
    facts.list_input_devices = []
    {
        return std::vector<std::string>{
            std::string{ firstInputDevicePath },
            std::string{ secondInputDevicePath },
        };
    };
    facts.path_readable = []( const std::string& path )
    {
        return path == firstInputDevicePath;
    };

    const grab::core::Environment env = grab::core::probe_environment( facts );
    ASSERT_EQ( env.input_devices.size(), expectedInputDeviceCount );
    EXPECT_TRUE( env.input_devices.at( firstInputDeviceIndex ).readable );
    EXPECT_FALSE( env.input_devices.at( secondInputDeviceIndex ).readable );
}
