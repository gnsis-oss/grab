#include "core/environment.hpp"
#include "core/fake_provider.hpp"
#include "grab/capability.hpp"
#include "grab/result.hpp"
#include "kernel/routing/provider.hpp"
#include "kernel/routing/registry.hpp"
#include "kernel/routing/resolver.hpp"

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace
{

    constexpr int                        qualityLow  = 10;
    constexpr int                        qualityHigh = 50;
    constexpr auto                       cap = grab::Capability::ScreenWindowImage;
    constexpr std::string_view           degradedHighName           = "degraded-hi";
    constexpr std::string_view           availableLowName           = "avail-lo";
    constexpr std::string_view           availableHighName          = "avail-hi";
    constexpr std::string_view           needsPermissionName        = "exact";
    constexpr std::string_view           fallbackName               = "fallback";
    constexpr std::string_view           unavailableName            = "gone";
    constexpr std::string_view           unavailableReason          = "probe said no";
    constexpr std::string_view           targetClass                = "window";
    constexpr std::string_view           flipperName                = "flipper";
    constexpr std::string_view           flippedReason              = "flipped";
    constexpr auto                       expectedProviderCount      = 3U;
    constexpr auto                       firstProviderIndex         = 0U;
    constexpr auto                       secondProviderIndex        = 1U;
    constexpr auto                       thirdProviderIndex         = 2U;
    constexpr auto                       expectedAttemptCount       = 1U;
    constexpr auto                       firstAttemptIndex          = 0U;
    constexpr auto                       firstGeneration            = 1U;
    constexpr auto                       secondGeneration           = 2U;
    constexpr int                        firstGenerationProbeCount  = 1;
    constexpr int                        secondGenerationProbeCount = 2;
    constexpr grab::core::ResolveOptions defaultOptions{};

    using ProviderSpec = std::tuple<std::string, grab::AvailabilityState, int>;

    grab::core::Registry
    make_registry( std::vector<ProviderSpec> specs )
    {
        grab::core::RegistryBuilder builder;
        for( auto& [name, state, quality] : specs )
        {
            builder.add( std::make_unique<grab::test::FakeProvider>(
                name,
                std::vector<grab::Capability>{ cap },
                grab::Availability{
                    .state   = state,
                    .reason  = state == grab::AvailabilityState::Available
                                 ? ""
                                 : std::string{ unavailableReason },
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
                     std::string{ degradedHighName },
                     grab::AvailabilityState::Degraded,
                     qualityHigh},
        ProviderSpec{
                     std::string{ availableLowName },
                     grab::AvailabilityState::Available,
                     qualityLow },
        ProviderSpec{
                     std::string{ availableHighName },
                     grab::AvailabilityState::Available,
                     qualityHigh},
    } );
    const grab::core::Resolver resolver( registry );

    const auto                 resolution = resolver.resolve(
        grab::core::CapabilityRequest{
            .capability   = cap,
            .target_class = std::string{ targetClass },
            .target_key   = "",
            .options      = defaultOptions,
        },
        grab::core::Environment{}
    );
    ASSERT_TRUE( resolution.has_value() );
    ASSERT_EQ( resolution->chain.size(), expectedProviderCount );
    EXPECT_EQ( resolution->chain.at( firstProviderIndex )->info().name,
               availableHighName );
    EXPECT_EQ( resolution->chain.at( secondProviderIndex )->info().name,
               availableLowName );
    EXPECT_EQ( resolution->chain.at( thirdProviderIndex )->info().name,
               degradedHighName );
}

TEST( Resolver,
      ExcludesUnavailableAndErrorsWhenChainEmpty )
{
    const auto                 registry = make_registry( {
        ProviderSpec{
                     std::string{ unavailableName },
                     grab::AvailabilityState::Unavailable,
                     qualityHigh
        },
    } );
    const grab::core::Resolver resolver( registry );

    const auto                 resolution = resolver.resolve(
        grab::core::CapabilityRequest{
            .capability   = cap,
            .target_class = std::string{ targetClass },
            .target_key   = "",
            .options      = defaultOptions,
        },
        grab::core::Environment{}
    );
    ASSERT_FALSE( resolution.has_value() );
    EXPECT_EQ( resolution.error().code, grab::ErrorCode::CapabilityUnavailable );
    EXPECT_EQ( resolution.error().target, targetClass );
    ASSERT_EQ( resolution.error().attempts.size(), expectedAttemptCount );
    EXPECT_EQ( resolution.error().attempts.at( firstAttemptIndex ).provider,
               unavailableName );
    EXPECT_EQ( resolution.error().attempts.at( firstAttemptIndex ).reason,
               unavailableReason );
}

TEST( Resolver,
      PermissionPreferenceOrdersChain )
{
    const auto                 registry = make_registry( {
        ProviderSpec{
                     std::string{ needsPermissionName },
                     grab::AvailabilityState::NeedsPermission,
                     qualityHigh},
        ProviderSpec{
                     std::string{ fallbackName },
                     grab::AvailabilityState::Degraded,
                     qualityHigh},
    } );
    const grab::core::Resolver resolver( registry );

    auto                       resolution = resolver.resolve(
        grab::core::CapabilityRequest{
            .capability   = cap,
            .target_class = "",
            .target_key   = "",
            .options      = defaultOptions,
        },
        grab::core::Environment{}
    );
    ASSERT_TRUE( resolution.has_value() );
    EXPECT_EQ( resolution->chain.at( firstProviderIndex )->info().name,
               needsPermissionName );

    const grab::core::ResolveOptions prompt_averse_options{
        .prefer_permission_over_degraded = false,
    };
    resolution = resolver.resolve(
        grab::core::CapabilityRequest{
            .capability   = cap,
            .target_class = "",
            .target_key   = "",
            .options      = prompt_averse_options,
        },
        grab::core::Environment{}
    );
    ASSERT_TRUE( resolution.has_value() );
    EXPECT_EQ( resolution->chain.at( firstProviderIndex )->info().name, fallbackName );
}

TEST( Resolver,
      CachesPerGenerationAndReprobesOnBump )
{
    class FlippingProvider final : public grab::core::Provider
    {
        public:

            FlippingProvider() :
                info_{
                    .name         = std::string{ flipperName },
                    .capabilities = std::vector<grab::Capability>{ cap },
                    .quality      = qualityLow,
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
                if( probes_ == firstGenerationProbeCount )
                {
                    return {
                        .state   = grab::AvailabilityState::Available,
                        .reason  = "",
                        .quality = qualityLow,
                    };
                }
                return {
                    .state   = grab::AvailabilityState::Degraded,
                    .reason  = std::string{ flippedReason },
                    .quality = qualityLow,
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
    env.generation = firstGeneration;
    ASSERT_TRUE( resolver
                     .resolve(
                         grab::core::CapabilityRequest{
                             .capability   = cap,
                             .target_class = "",
                             .target_key   = "",
                             .options      = defaultOptions,
                         },
                         env
                     )
                     .has_value() );
    ASSERT_TRUE( resolver
                     .resolve(
                         grab::core::CapabilityRequest{
                             .capability   = cap,
                             .target_class = "",
                             .target_key   = "",
                             .options      = defaultOptions,
                         },
                         env
                     )
                     .has_value() );
    EXPECT_EQ( flipper->probes(), firstGenerationProbeCount );

    env.generation        = secondGeneration;
    const auto resolution = resolver.resolve(
        grab::core::CapabilityRequest{
            .capability   = cap,
            .target_class = "",
            .target_key   = "",
            .options      = defaultOptions,
        },
        env
    );
    ASSERT_TRUE( resolution.has_value() );
    EXPECT_EQ( resolution->best.state, grab::AvailabilityState::Degraded );
    EXPECT_EQ( resolution->best.reason, flippedReason );
    EXPECT_EQ( flipper->probes(), secondGenerationProbeCount );
    ASSERT_TRUE( resolver
                     .resolve(
                         grab::core::CapabilityRequest{
                             .capability   = cap,
                             .target_class = "",
                             .target_key   = "",
                             .options      = defaultOptions,
                         },
                         env
                     )
                     .has_value() );
    EXPECT_EQ( flipper->probes(), secondGenerationProbeCount );
}
