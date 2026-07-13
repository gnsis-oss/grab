#include "core/environment.hpp"
#include "grab/capability.hpp"
#include "grab/workspace.hpp"
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
        env.session = grab::core::SessionType::X11;
        return env;
    }

}    // namespace

TEST( X11SeatSessionProvider,
      SharedAvailableOnX11 )
{
    const grab::session::X11SeatSessionProvider provider;
    const auto av = provider.probe( x11_env(), grab::WorkspaceMode::Shared );
    EXPECT_EQ( av.state, grab::AvailabilityState::Available );
}

TEST( X11SeatSessionProvider,
      OffscreenUnavailableOnX11 )
{
    const grab::session::X11SeatSessionProvider provider;
    const auto av = provider.probe( x11_env(), grab::WorkspaceMode::Offscreen );
    EXPECT_EQ( av.state, grab::AvailabilityState::Unavailable );
    EXPECT_FALSE( av.reason.empty() );
}

TEST( X11SeatSessionProvider,
      UnavailableOffX11 )
{
    const grab::core::Environment               non_x11;
    const grab::session::X11SeatSessionProvider provider;
    const auto av = provider.probe( non_x11, grab::WorkspaceMode::Shared );
    EXPECT_EQ( av.state, grab::AvailabilityState::Unavailable );
}
