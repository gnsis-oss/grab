#include "core/environment.hpp"
#include "grab/session.hpp"
#include "session/builtin_session_providers.hpp"
#include "session/selection.hpp"

// clang-format off
#include <gtest/gtest.h>
// clang-format on

TEST( BuiltinSessionProviders,
      ReportsX11OffscreenUnavailableSharedAvailable )
{
    grab::core::Environment env;
    env.session          = grab::core::SessionType::X11;

    const auto providers = grab::session::builtin_session_providers();
    ASSERT_FALSE( providers.empty() );

    const auto offscreen =
        grab::session::select_session_provider( providers,
                                                env,
                                                grab::SessionMode::Offscreen );
    EXPECT_FALSE( offscreen.has_value() );    // spec section 9: no X11 offscreen

    const auto shared =
        grab::session::select_session_provider( providers,
                                                env,
                                                grab::SessionMode::Shared );
    EXPECT_TRUE( shared.has_value() );
}
