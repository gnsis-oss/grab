#include "core/environment.hpp"
#include "core/fake_provider.hpp"
#include "grab/capability.hpp"
#include "kernel/routing/doctor.hpp"
#include "kernel/routing/registry.hpp"

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

    constexpr int              quality                 = 10;
    constexpr auto             expectedCapabilityCount = 1U;
    constexpr auto             firstCapabilityIndex    = 0U;
    constexpr int              healthyExitCode         = 0;
    constexpr int              unhealthyExitCode       = 1;
    constexpr auto             exactJsonGeneration     = 7U;
    constexpr std::string_view screenProviderName      = "fake-screen";
    constexpr auto             capability   = grab::Capability::ScreenWindowImage;
    constexpr std::string_view capabilityId = grab::capability::screen_window_image;
    constexpr std::string_view emptyReason{};
    constexpr std::string_view degradedReason        = "visible region only";
    constexpr std::string_view escapedDesktop        = R"(KDE "test")";
    constexpr std::string_view controlDesktop        = "\b\f\x1b";
    constexpr std::string_view sessionJson           = R"("session":"x11")";
    constexpr std::string_view desktopJson           = R"("desktop":"KDE \"test\"")";
    constexpr std::string_view controlDesktopJson    = R"(\b\f\u001b)";
    constexpr std::string_view emptyCapabilitiesJson = R"("capabilities":[])";

    grab::core::Registry
    registry_with_one_screen_provider( grab::AvailabilityState state,
                                       std::string_view        reason )
    {
        grab::core::RegistryBuilder builder;
        builder.add( std::make_unique<grab::test::FakeProvider>(
            std::string{ screenProviderName },
            std::vector<grab::Capability>{ capability },
            grab::Availability{
                .state   = state,
                .reason  = std::string{ reason },
                .quality = quality,
            }
        ) );
        return std::move( builder ).build();
    }

}    // namespace

TEST( Doctor,
      ReportsBestProviderPerCapability )
{
    const auto registry =
        registry_with_one_screen_provider( grab::AvailabilityState::Available,
                                           emptyReason );
    const auto report = grab::core::run_doctor( registry, grab::core::Environment{} );

    ASSERT_EQ( report.capabilities.size(), expectedCapabilityCount );
    EXPECT_EQ( report.capabilities.at( firstCapabilityIndex ).id, capabilityId );
    EXPECT_EQ( report.capabilities.at( firstCapabilityIndex ).provider,
               screenProviderName );
    EXPECT_EQ( report.capabilities.at( firstCapabilityIndex ).state,
               grab::AvailabilityState::Available );
    EXPECT_EQ( grab::core::doctor_exit_code( report ), healthyExitCode );
}

TEST( Doctor,
      DegradedCapabilityYieldsExitCodeOne )
{
    const auto registry =
        registry_with_one_screen_provider( grab::AvailabilityState::Degraded,
                                           degradedReason );
    const auto report = grab::core::run_doctor( registry, grab::core::Environment{} );
    EXPECT_EQ( grab::core::doctor_exit_code( report ), unhealthyExitCode );
}

TEST( Doctor,
      EmptyRegistryIsVacuouslyHealthy )
{
    grab::core::RegistryBuilder builder;
    const auto                  registry = std::move( builder ).build();
    const auto report = grab::core::run_doctor( registry, grab::core::Environment{} );
    EXPECT_TRUE( report.capabilities.empty() );
    EXPECT_EQ( grab::core::doctor_exit_code( report ), healthyExitCode );
}

TEST( Doctor,
      JsonIsDeterministicAndEscaped )
{
    grab::core::Environment env;
    env.session = grab::core::SessionType::X11;
    env.desktop = escapedDesktop;
    grab::core::RegistryBuilder builder;
    const auto                  registry = std::move( builder ).build();

    const auto json = grab::core::to_json( grab::core::run_doctor( registry, env ) );
    EXPECT_NE( json.find( sessionJson ), std::string::npos );
    EXPECT_NE( json.find( desktopJson ), std::string::npos );
    EXPECT_NE( json.find( emptyCapabilitiesJson ), std::string::npos );
}

TEST( Doctor,
      JsonEscapesC0ControlCharacters )
{
    grab::core::Environment env;
    env.desktop = controlDesktop;
    grab::core::RegistryBuilder builder;
    const auto                  registry = std::move( builder ).build();

    const auto json = grab::core::to_json( grab::core::run_doctor( registry, env ) );
    EXPECT_NE( json.find( controlDesktopJson ), std::string::npos );
}

TEST( Doctor,
      JsonIsExactAndParsesAsValidJson )
{
    constexpr std::string_view expectedJson =
        R"({"environment":{"session":"x11","xwayland":false,"desktop":"KDE","generation":7,"uinput_writable":true,"input_devices":[{"path":"/dev/input/event0","readable":true}]},"capabilities":[]})";

    grab::core::Environment env;
    env.session          = grab::core::SessionType::X11;
    env.xwayland_present = false;
    env.desktop          = "KDE";
    env.generation       = exactJsonGeneration;
    env.uinput_writable  = true;
    env.input_devices    = {
        grab::core::InputDeviceAccess{
                                      .path     = "/dev/input/event0",
                                      .readable = true,
                                      },
    };

    grab::core::RegistryBuilder builder;
    const auto                  registry = std::move( builder ).build();
    const auto                  report   = grab::core::run_doctor( registry, env );

    EXPECT_EQ( grab::core::to_json( report ), expectedJson );
}
