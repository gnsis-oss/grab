#include "core/doctor.hpp"
#include "core/environment.hpp"
#include "core/fake_provider.hpp"
#include "core/registry.hpp"
#include "grab/capability.hpp"

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

    constexpr int              kQuality                 = 10;
    constexpr auto             kExpectedCapabilityCount = 1U;
    constexpr auto             kFirstCapabilityIndex    = 0U;
    constexpr int              kHealthyExitCode         = 0;
    constexpr int              kUnhealthyExitCode       = 1;
    constexpr auto             kExactJsonGeneration     = 7U;
    constexpr std::string_view kScreenProviderName      = "fake-screen";
    constexpr auto             kCapability   = grab::Capability::screen_window_image;
    constexpr std::string_view kCapabilityId = grab::capability::screen_window_image;
    constexpr std::string_view kEmptyReason{};
    constexpr std::string_view kDegradedReason        = "visible region only";
    constexpr std::string_view kEscapedDesktop        = R"(KDE "test")";
    constexpr std::string_view kControlDesktop        = "\b\f\x1b";
    constexpr std::string_view kSessionJson           = R"("session":"x11")";
    constexpr std::string_view kDesktopJson           = R"("desktop":"KDE \"test\"")";
    constexpr std::string_view kControlDesktopJson    = R"(\u0008\u000c\u001b)";
    constexpr std::string_view kEmptyCapabilitiesJson = R"("capabilities":[])";

    grab::core::Registry
    registry_with_one_screen_provider( grab::AvailabilityState state,
                                       std::string_view        reason )
    {
        grab::core::RegistryBuilder builder;
        builder.add( std::make_unique<grab::test::FakeProvider>(
            std::string{ kScreenProviderName },
            std::vector<grab::Capability>{ kCapability },
            grab::Availability{
                .state   = state,
                .reason  = std::string{ reason },
                .quality = kQuality,
            }
        ) );
        return std::move( builder ).build();
    }

}    // namespace

TEST( Doctor,
      ReportsBestProviderPerCapability )
{
    const auto registry =
        registry_with_one_screen_provider( grab::AvailabilityState::available,
                                           kEmptyReason );
    const auto report = grab::core::run_doctor( registry, grab::core::Environment{} );

    ASSERT_EQ( report.capabilities.size(), kExpectedCapabilityCount );
    EXPECT_EQ( report.capabilities.at( kFirstCapabilityIndex ).id, kCapabilityId );
    EXPECT_EQ( report.capabilities.at( kFirstCapabilityIndex ).provider,
               kScreenProviderName );
    EXPECT_EQ( report.capabilities.at( kFirstCapabilityIndex ).state,
               grab::AvailabilityState::available );
    EXPECT_EQ( grab::core::doctor_exit_code( report ), kHealthyExitCode );
}

TEST( Doctor,
      DegradedCapabilityYieldsExitCodeOne )
{
    const auto registry =
        registry_with_one_screen_provider( grab::AvailabilityState::degraded,
                                           kDegradedReason );
    const auto report = grab::core::run_doctor( registry, grab::core::Environment{} );
    EXPECT_EQ( grab::core::doctor_exit_code( report ), kUnhealthyExitCode );
}

TEST( Doctor,
      EmptyRegistryIsVacuouslyHealthy )
{
    grab::core::RegistryBuilder builder;
    const auto                  registry = std::move( builder ).build();
    const auto report = grab::core::run_doctor( registry, grab::core::Environment{} );
    EXPECT_TRUE( report.capabilities.empty() );
    EXPECT_EQ( grab::core::doctor_exit_code( report ), kHealthyExitCode );
}

TEST( Doctor,
      JsonIsDeterministicAndEscaped )
{
    grab::core::Environment env;
    env.session = grab::core::SessionType::x11;
    env.desktop = kEscapedDesktop;
    grab::core::RegistryBuilder builder;
    const auto                  registry = std::move( builder ).build();

    const auto json = grab::core::to_json( grab::core::run_doctor( registry, env ) );
    EXPECT_NE( json.find( kSessionJson ), std::string::npos );
    EXPECT_NE( json.find( kDesktopJson ), std::string::npos );
    EXPECT_NE( json.find( kEmptyCapabilitiesJson ), std::string::npos );
}

TEST( Doctor,
      JsonEscapesC0ControlCharacters )
{
    grab::core::Environment env;
    env.desktop = kControlDesktop;
    grab::core::RegistryBuilder builder;
    const auto                  registry = std::move( builder ).build();

    const auto json = grab::core::to_json( grab::core::run_doctor( registry, env ) );
    EXPECT_NE( json.find( kControlDesktopJson ), std::string::npos );
}

TEST( Doctor,
      JsonIsExactAndParsesAsValidJson )
{
    constexpr std::string_view kExpectedJson =
        R"({"environment":{"session":"x11","xwayland":false,"desktop":"KDE","generation":7,"uinput_writable":true,"input_devices":[{"path":"/dev/input/event0","readable":true}]},"capabilities":[]})";

    grab::core::Environment env;
    env.session          = grab::core::SessionType::x11;
    env.xwayland_present = false;
    env.desktop          = "KDE";
    env.generation       = kExactJsonGeneration;
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

    EXPECT_EQ( grab::core::to_json( report ), kExpectedJson );
}
