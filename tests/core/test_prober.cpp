#include "core/environment.hpp"
#include "core/prober.hpp"

#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

    constexpr std::string_view kDisplayName              = "DISPLAY";
    constexpr std::string_view kDisplayValue             = ":0";
    constexpr std::string_view kSessionTypeName          = "XDG_SESSION_TYPE";
    constexpr std::string_view kX11SessionTypeValue      = "x11";
    constexpr std::string_view kWaylandDisplayName       = "WAYLAND_DISPLAY";
    constexpr std::string_view kWaylandDisplayValue      = "wayland-0";
    constexpr std::string_view kCurrentDesktopName       = "XDG_CURRENT_DESKTOP";
    constexpr std::string_view kKdeDesktopValue          = "KDE";
    constexpr std::string_view kUinputPath               = "/dev/uinput";
    constexpr std::string_view kFirstInputDevicePath     = "/dev/input/event0";
    constexpr std::string_view kSecondInputDevicePath    = "/dev/input/event1";
    constexpr auto             kExpectedInputDeviceCount = 2U;
    constexpr auto             kFirstInputDeviceIndex    = 0U;
    constexpr auto             kSecondInputDeviceIndex   = 1U;

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
        env_var( kDisplayName, kDisplayValue ),
        env_var( kSessionTypeName, kX11SessionTypeValue ),
    } ) );
    EXPECT_EQ( env.session, grab::core::SessionType::x11 );
    EXPECT_FALSE( env.xwayland_present );
}

TEST( Prober,
      DetectsWaylandWithXwayland )
{
    const grab::core::Environment env = grab::core::probe_environment( facts_with_env( {
        env_var( kWaylandDisplayName, kWaylandDisplayValue ),
        env_var( kDisplayName, kDisplayValue ),
        env_var( kCurrentDesktopName, kKdeDesktopValue ),
    } ) );
    EXPECT_EQ( env.session, grab::core::SessionType::wayland );
    EXPECT_TRUE( env.xwayland_present );
    EXPECT_EQ( env.desktop, kKdeDesktopValue );
}

TEST( Prober,
      DetectsPureWaylandWithoutXwayland )
{
    const grab::core::Environment env = grab::core::probe_environment( facts_with_env( {
        env_var( kWaylandDisplayName, kWaylandDisplayValue ),
    } ) );
    EXPECT_EQ( env.session, grab::core::SessionType::wayland );
    EXPECT_FALSE( env.xwayland_present );
}

TEST( Prober,
      DetectsHeadless )
{
    const grab::core::Environment env =
        grab::core::probe_environment( facts_with_env( {} ) );
    EXPECT_EQ( env.session, grab::core::SessionType::headless );
}

TEST( Prober,
      ProbesUinputWriteAccess )
{
    grab::core::SystemFacts writable =
        facts_with_env( { env_var( kDisplayName, kDisplayValue ) } );
    writable.path_writable = []( const std::string& path )
    {
        return path == std::string{ kUinputPath };
    };

    const grab::core::Environment writable_env =
        grab::core::probe_environment( writable );
    EXPECT_TRUE( writable_env.uinput_writable );

    grab::core::SystemFacts unwritable =
        facts_with_env( { env_var( kDisplayName, kDisplayValue ) } );
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
        facts_with_env( { env_var( kDisplayName, kDisplayValue ) } );
    facts.list_input_devices = []
    {
        return std::vector<std::string>{
            std::string{ kFirstInputDevicePath },
            std::string{ kSecondInputDevicePath },
        };
    };
    facts.path_readable = []( const std::string& path )
    {
        return path == kFirstInputDevicePath;
    };

    const grab::core::Environment env = grab::core::probe_environment( facts );
    ASSERT_EQ( env.input_devices.size(), kExpectedInputDeviceCount );
    EXPECT_TRUE( env.input_devices.at( kFirstInputDeviceIndex ).readable );
    EXPECT_FALSE( env.input_devices.at( kSecondInputDeviceIndex ).readable );
}
