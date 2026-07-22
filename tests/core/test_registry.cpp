#include "core/fake_provider.hpp"
#include "grab/capability.hpp"
#include "kernel/routing/registry.hpp"

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

    constexpr int              highQuality         = 90;
    constexpr std::string_view screenProviderName  = "fake-screen";
    constexpr std::string_view inputProviderName   = "fake-input";
    constexpr auto             expectedScreenCount = 1U;
    constexpr auto             expectedAllCount    = 2U;
    constexpr auto             firstScreenProvider = 0U;
    constexpr auto             firstProvider       = 0U;
    constexpr auto             secondProvider      = 1U;

    grab::Availability
    available( int quality )
    {
        return {
            .state   = grab::AvailabilityState::Available,
            .reason  = "",
            .quality = quality,
        };
    }

}    // namespace

TEST( Registry,
      FiltersProvidersByCapability )
{
    grab::core::RegistryBuilder builder;
    builder.add( std::make_unique<grab::test::FakeProvider>(
        std::string{ screenProviderName },
        std::vector<grab::Capability>{
            grab::Capability::ScreenWindowImage,
        },
        available( highQuality )
    ) );
    builder.add(
        std::make_unique<grab::test::FakeProvider>( std::string{ inputProviderName },
                                                    std::vector<grab::Capability>{
                                                        grab::Capability::MouseClick,
                                                    },
                                                    available( highQuality ) )
    );
    const auto registry = std::move( builder ).build();

    const auto screen   = registry.providers_for( grab::Capability::ScreenWindowImage );
    ASSERT_EQ( screen.size(), expectedScreenCount );
    EXPECT_EQ( screen.at( firstScreenProvider )->info().name, screenProviderName );
    EXPECT_TRUE( registry.providers_for( grab::Capability::KeyChord ).empty() );
}

TEST( Registry,
      AllReturnsRegisteredProviders )
{
    grab::core::RegistryBuilder builder;
    builder.add( std::make_unique<grab::test::FakeProvider>(
        std::string{ screenProviderName },
        std::vector<grab::Capability>{
            grab::Capability::ScreenWindowImage,
        },
        available( highQuality )
    ) );
    builder.add(
        std::make_unique<grab::test::FakeProvider>( std::string{ inputProviderName },
                                                    std::vector<grab::Capability>{
                                                        grab::Capability::MouseClick,
                                                    },
                                                    available( highQuality ) )
    );
    const auto registry  = std::move( builder ).build();

    const auto providers = registry.all();
    ASSERT_EQ( providers.size(), expectedAllCount );
    EXPECT_EQ( providers.at( firstProvider )->info().name, screenProviderName );
    EXPECT_EQ( providers.at( secondProvider )->info().name, inputProviderName );
}
