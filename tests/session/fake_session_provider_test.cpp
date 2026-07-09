#include "grab/result.hpp"
#include "grab/session.hpp"
#include "session/fake_session_provider.hpp"

// clang-format off
#include <gtest/gtest.h>
// clang-format on

namespace
{

    constexpr auto                  session_name          = "ai";
    constexpr auto                  failure_message       = "boom";
    constexpr auto                  expected_create_calls = 1U;
    constexpr grab::SessionGeometry default_geometry{};

    [[nodiscard]]
    grab::SessionDesc
    desc()
    {
        return grab::SessionDesc{
            .name        = session_name,
            .mode        = grab::SessionMode::offscreen,
            .geometry    = default_geometry,
            .app_command = {},
        };
    }

}    // namespace

TEST( FakeSessionProvider,
      CreateReturnsRuntimeAndRecords )
{
    const grab::test::FakeSessionProvider provider;
    const auto                            runtime = provider.create( desc() );

    ASSERT_TRUE( runtime.has_value() ) << runtime.error().message;
    EXPECT_FALSE( runtime->endpoint.empty() );
    EXPECT_EQ( provider.create_calls(), expected_create_calls );
}

TEST( FakeSessionProvider,
      CanBeConfiguredToFail )
{
    grab::test::FakeSessionProvider provider;
    provider.fail_next_create( grab::ErrorCode::provider_failed, failure_message );
    const auto runtime = provider.create( desc() );

    ASSERT_FALSE( runtime.has_value() );
    EXPECT_EQ( runtime.error().code, grab::ErrorCode::provider_failed );
}
