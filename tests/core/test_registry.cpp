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

    constexpr int              kHighQuality         = 90;
    constexpr std::string_view kScreenProviderName  = "fake-screen";
    constexpr std::string_view kInputProviderName   = "fake-input";
    constexpr auto             kExpectedScreenCount = 1U;
    constexpr auto             kExpectedAllCount    = 2U;
    constexpr auto             kFirstScreenProvider = 0U;
    constexpr auto             kFirstProvider       = 0U;
    constexpr auto             kSecondProvider      = 1U;

    grab::Availability
    available( int quality )
    {
        return {
            .state   = grab::AvailabilityState::available,
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
        std::string{ kScreenProviderName },
        std::vector<grab::Capability>{
            grab::Capability::screen_window_image,
        },
        available( kHighQuality )
    ) );
    builder.add(
        std::make_unique<grab::test::FakeProvider>( std::string{ kInputProviderName },
                                                    std::vector<grab::Capability>{
                                                        grab::Capability::mouse_click,
                                                    },
                                                    available( kHighQuality ) )
    );
    const auto registry = std::move( builder ).build();

    const auto screen = registry.providers_for( grab::Capability::screen_window_image );
    ASSERT_EQ( screen.size(), kExpectedScreenCount );
    EXPECT_EQ( screen.at( kFirstScreenProvider )->info().name, kScreenProviderName );
    EXPECT_TRUE( registry.providers_for( grab::Capability::key_chord ).empty() );
}

TEST( Registry,
      AllReturnsRegisteredProviders )
{
    grab::core::RegistryBuilder builder;
    builder.add( std::make_unique<grab::test::FakeProvider>(
        std::string{ kScreenProviderName },
        std::vector<grab::Capability>{
            grab::Capability::screen_window_image,
        },
        available( kHighQuality )
    ) );
    builder.add(
        std::make_unique<grab::test::FakeProvider>( std::string{ kInputProviderName },
                                                    std::vector<grab::Capability>{
                                                        grab::Capability::mouse_click,
                                                    },
                                                    available( kHighQuality ) )
    );
    const auto registry  = std::move( builder ).build();

    const auto providers = registry.all();
    ASSERT_EQ( providers.size(), kExpectedAllCount );
    EXPECT_EQ( providers.at( kFirstProvider )->info().name, kScreenProviderName );
    EXPECT_EQ( providers.at( kSecondProvider )->info().name, kInputProviderName );
}
