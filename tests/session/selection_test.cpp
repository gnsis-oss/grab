#include "core/environment.hpp"
#include "grab/capability.hpp"
#include "grab/result.hpp"
#include "grab/session.hpp"
#include "session/fake_session_provider.hpp"
#include "session/provider.hpp"
#include "session/selection.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
// clang-format on

namespace
{

    using grab::AvailabilityState;
    using grab::SessionMode;

    constexpr int              low_quality         = 10;
    constexpr int              high_quality        = 90;
    constexpr std::string_view no_offscreen_reason = "no offscreen on X11";
    constexpr std::size_t      first_provider      = 0U;
    constexpr std::size_t      second_provider     = 1U;

    [[nodiscard]]
    grab::Availability
    availability( AvailabilityState state,
                  int               quality,
                  std::string_view  reason = {} )
    {
        grab::Availability value;
        value.state   = state;
        value.reason  = std::string{ reason };
        value.quality = quality;
        return value;
    }

}    // namespace

TEST( SelectSessionProvider,
      PicksHighestQualityAvailable )
{
    grab::test::FakeSessionProvider weak{ "weak" };
    grab::test::FakeSessionProvider strong{ "strong" };
    weak.set_availability( SessionMode::shared,
                           availability( AvailabilityState::available, low_quality ) );
    strong.set_availability( SessionMode::shared,
                             availability( AvailabilityState::available,
                                           high_quality ) );

    std::array<const grab::session::SessionProvider*, 2U> providers{};
    providers.at( first_provider )  = &weak;
    providers.at( second_provider ) = &strong;
    const grab::core::Environment env;

    const auto                    chosen =
        grab::session::select_session_provider( providers, env, SessionMode::shared );

    ASSERT_TRUE( chosen.has_value() ) << chosen.error().message;
    EXPECT_EQ( ( *chosen )->info().name, "strong" );
}

TEST( SelectSessionProvider,
      UnavailableModeIsCapabilityUnavailable )
{
    grab::test::FakeSessionProvider only_shared{ "x11" };
    only_shared.set_availability( SessionMode::shared,
                                  availability( AvailabilityState::available,
                                                high_quality ) );
    only_shared.set_availability(
        SessionMode::offscreen,
        availability( AvailabilityState::unavailable, 0, no_offscreen_reason )
    );

    std::array<const grab::session::SessionProvider*, 1U> providers{};
    providers.at( first_provider ) = &only_shared;
    const grab::core::Environment env;

    const auto                    chosen =
        grab::session::select_session_provider( providers, env, SessionMode::offscreen );

    ASSERT_FALSE( chosen.has_value() );
    EXPECT_EQ( chosen.error().code, grab::ErrorCode::capability_unavailable );
}
