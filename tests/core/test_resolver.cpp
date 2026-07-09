#include "core/environment.hpp"
#include "core/fake_provider.hpp"
#include "core/provider.hpp"
#include "core/registry.hpp"
#include "core/resolver.hpp"
#include "grab/capability.hpp"
#include "grab/result.hpp"

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace
{

    constexpr int                        kQualityLow  = 10;
    constexpr int                        kQualityHigh = 50;
    constexpr auto                       kCap = grab::Capability::screen_window_image;
    constexpr std::string_view           kDegradedHighName           = "degraded-hi";
    constexpr std::string_view           kAvailableLowName           = "avail-lo";
    constexpr std::string_view           kAvailableHighName          = "avail-hi";
    constexpr std::string_view           kNeedsPermissionName        = "exact";
    constexpr std::string_view           kFallbackName               = "fallback";
    constexpr std::string_view           kUnavailableName            = "gone";
    constexpr std::string_view           kUnavailableReason          = "probe said no";
    constexpr std::string_view           kTargetClass                = "window";
    constexpr std::string_view           kFlipperName                = "flipper";
    constexpr std::string_view           kFlippedReason              = "flipped";
    constexpr auto                       kExpectedProviderCount      = 3U;
    constexpr auto                       kFirstProviderIndex         = 0U;
    constexpr auto                       kSecondProviderIndex        = 1U;
    constexpr auto                       kThirdProviderIndex         = 2U;
    constexpr auto                       kExpectedAttemptCount       = 1U;
    constexpr auto                       kFirstAttemptIndex          = 0U;
    constexpr auto                       kFirstGeneration            = 1U;
    constexpr auto                       kSecondGeneration           = 2U;
    constexpr int                        kFirstGenerationProbeCount  = 1;
    constexpr int                        kSecondGenerationProbeCount = 2;
    constexpr grab::core::ResolveOptions kDefaultOptions{};

    using ProviderSpec = std::tuple<std::string, grab::AvailabilityState, int>;

    grab::core::Registry
    make_registry( std::vector<ProviderSpec> specs )
    {
        grab::core::RegistryBuilder builder;
        for( auto& [name, state, quality] : specs )
        {
            builder.add( std::make_unique<grab::test::FakeProvider>(
                name,
                std::vector<grab::Capability>{ kCap },
                grab::Availability{
                    .state   = state,
                    .reason  = state == grab::AvailabilityState::available
                                 ? ""
                                 : std::string{ kUnavailableReason },
                    .quality = quality,
                }
            ) );
        }
        return std::move( builder ).build();
    }

}    // namespace

TEST( Resolver,
      PrefersAvailableOverDegradedThenQuality )
{
    const auto                 registry = make_registry( {
        ProviderSpec{
                     std::string{ kDegradedHighName },
                     grab::AvailabilityState::degraded,
                     kQualityHigh},
        ProviderSpec{
                     std::string{ kAvailableLowName },
                     grab::AvailabilityState::available,
                     kQualityLow },
        ProviderSpec{
                     std::string{ kAvailableHighName },
                     grab::AvailabilityState::available,
                     kQualityHigh},
    } );
    const grab::core::Resolver resolver( registry );

    const auto                 resolution = resolver.resolve(
        grab::core::CapabilityRequest{
            .capability   = kCap,
            .target_class = std::string{ kTargetClass },
            .target_key   = "",
            .options      = kDefaultOptions,
        },
        grab::core::Environment{}
    );
    ASSERT_TRUE( resolution.has_value() );
    ASSERT_EQ( resolution->chain.size(), kExpectedProviderCount );
    EXPECT_EQ( resolution->chain.at( kFirstProviderIndex )->info().name,
               kAvailableHighName );
    EXPECT_EQ( resolution->chain.at( kSecondProviderIndex )->info().name,
               kAvailableLowName );
    EXPECT_EQ( resolution->chain.at( kThirdProviderIndex )->info().name,
               kDegradedHighName );
}

TEST( Resolver,
      ExcludesUnavailableAndErrorsWhenChainEmpty )
{
    const auto                 registry = make_registry( {
        ProviderSpec{
                     std::string{ kUnavailableName },
                     grab::AvailabilityState::unavailable,
                     kQualityHigh
        },
    } );
    const grab::core::Resolver resolver( registry );

    const auto                 resolution = resolver.resolve(
        grab::core::CapabilityRequest{
            .capability   = kCap,
            .target_class = std::string{ kTargetClass },
            .target_key   = "",
            .options      = kDefaultOptions,
        },
        grab::core::Environment{}
    );
    ASSERT_FALSE( resolution.has_value() );
    EXPECT_EQ( resolution.error().code, grab::ErrorCode::capability_unavailable );
    EXPECT_EQ( resolution.error().target, kTargetClass );
    ASSERT_EQ( resolution.error().attempts.size(), kExpectedAttemptCount );
    EXPECT_EQ( resolution.error().attempts.at( kFirstAttemptIndex ).provider,
               kUnavailableName );
    EXPECT_EQ( resolution.error().attempts.at( kFirstAttemptIndex ).reason,
               kUnavailableReason );
}

TEST( Resolver,
      PermissionPreferenceOrdersChain )
{
    const auto                 registry = make_registry( {
        ProviderSpec{
                     std::string{ kNeedsPermissionName },
                     grab::AvailabilityState::needs_permission,
                     kQualityHigh},
        ProviderSpec{
                     std::string{ kFallbackName },
                     grab::AvailabilityState::degraded,
                     kQualityHigh},
    } );
    const grab::core::Resolver resolver( registry );

    auto                       resolution = resolver.resolve(
        grab::core::CapabilityRequest{
            .capability   = kCap,
            .target_class = "",
            .target_key   = "",
            .options      = kDefaultOptions,
        },
        grab::core::Environment{}
    );
    ASSERT_TRUE( resolution.has_value() );
    EXPECT_EQ( resolution->chain.at( kFirstProviderIndex )->info().name,
               kNeedsPermissionName );

    const grab::core::ResolveOptions prompt_averse_options{
        .prefer_permission_over_degraded = false,
    };
    resolution = resolver.resolve(
        grab::core::CapabilityRequest{
            .capability   = kCap,
            .target_class = "",
            .target_key   = "",
            .options      = prompt_averse_options,
        },
        grab::core::Environment{}
    );
    ASSERT_TRUE( resolution.has_value() );
    EXPECT_EQ( resolution->chain.at( kFirstProviderIndex )->info().name, kFallbackName );
}

TEST( Resolver,
      CachesPerGenerationAndReprobesOnBump )
{
    class FlippingProvider final : public grab::core::Provider
    {
        public:

            FlippingProvider() :
                info_{
                    .name         = std::string{ kFlipperName },
                    .capabilities = std::vector<grab::Capability>{ kCap },
                    .quality      = kQualityLow,
                }
            {
            }

            [[nodiscard]]
            const grab::core::ProviderInfo&
            info() const noexcept override
            {
                return info_;
            }

            [[nodiscard]]
            grab::Availability
            probe( const grab::core::Environment& /*env*/ ) const override
            {
                ++probes_;
                if( probes_ == kFirstGenerationProbeCount )
                {
                    return {
                        .state   = grab::AvailabilityState::available,
                        .reason  = "",
                        .quality = kQualityLow,
                    };
                }
                return {
                    .state   = grab::AvailabilityState::degraded,
                    .reason  = std::string{ kFlippedReason },
                    .quality = kQualityLow,
                };
            }

            [[nodiscard]]
            int
            probes() const
            {
                return probes_;
            }

        private:

            grab::core::ProviderInfo info_;
            mutable int              probes_ = 0;
    };

    grab::core::RegistryBuilder builder;
    auto                        owned   = std::make_unique<FlippingProvider>();
    const auto*                 flipper = owned.get();
    builder.add( std::move( owned ) );
    const auto                 registry = std::move( builder ).build();
    const grab::core::Resolver resolver( registry );

    grab::core::Environment    env;
    env.generation = kFirstGeneration;
    ASSERT_TRUE( resolver
                     .resolve(
                         grab::core::CapabilityRequest{
                             .capability   = kCap,
                             .target_class = "",
                             .target_key   = "",
                             .options      = kDefaultOptions,
                         },
                         env
                     )
                     .has_value() );
    ASSERT_TRUE( resolver
                     .resolve(
                         grab::core::CapabilityRequest{
                             .capability   = kCap,
                             .target_class = "",
                             .target_key   = "",
                             .options      = kDefaultOptions,
                         },
                         env
                     )
                     .has_value() );
    EXPECT_EQ( flipper->probes(), kFirstGenerationProbeCount );

    env.generation        = kSecondGeneration;
    const auto resolution = resolver.resolve(
        grab::core::CapabilityRequest{
            .capability   = kCap,
            .target_class = "",
            .target_key   = "",
            .options      = kDefaultOptions,
        },
        env
    );
    ASSERT_TRUE( resolution.has_value() );
    EXPECT_EQ( resolution->best.state, grab::AvailabilityState::degraded );
    EXPECT_EQ( resolution->best.reason, kFlippedReason );
    EXPECT_EQ( flipper->probes(), kSecondGenerationProbeCount );
    ASSERT_TRUE( resolver
                     .resolve(
                         grab::core::CapabilityRequest{
                             .capability   = kCap,
                             .target_class = "",
                             .target_key   = "",
                             .options      = kDefaultOptions,
                         },
                         env
                     )
                     .has_value() );
    EXPECT_EQ( flipper->probes(), kSecondGenerationProbeCount );
}
