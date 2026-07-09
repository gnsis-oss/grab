#include "core/environment.hpp"
#include "grab/capability.hpp"
#include "grab/session.hpp"
#include "session/x11_seat_provider.hpp"

// clang-format off
#include <gtest/gtest.h>
// clang-format on

namespace
{

    grab::core::Environment
    x11_env()
    {
        grab::core::Environment env;
        env.session = grab::core::SessionType::x11;
        return env;
    }

}    // namespace

TEST( X11SeatSessionProvider,
      SharedAvailableOnX11 )
{
    const grab::session::X11SeatSessionProvider provider;
    const auto av = provider.probe( x11_env(), grab::SessionMode::shared );
    EXPECT_EQ( av.state, grab::AvailabilityState::available );
}

TEST( X11SeatSessionProvider,
      OffscreenUnavailableOnX11 )
{
    const grab::session::X11SeatSessionProvider provider;
    const auto av = provider.probe( x11_env(), grab::SessionMode::offscreen );
    EXPECT_EQ( av.state, grab::AvailabilityState::unavailable );
    EXPECT_FALSE( av.reason.empty() );
}

TEST( X11SeatSessionProvider,
      UnavailableOffX11 )
{
    const grab::core::Environment               non_x11;
    const grab::session::X11SeatSessionProvider provider;
    const auto av = provider.probe( non_x11, grab::SessionMode::shared );
    EXPECT_EQ( av.state, grab::AvailabilityState::unavailable );
}
