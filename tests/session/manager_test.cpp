#include "grab/result.hpp"
#include "grab/session.hpp"
#include "session/fake_session_provider.hpp"
#include "session/manager.hpp"
#include "session/registry.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <filesystem>
#include <string>
#include <string_view>
// clang-format on

namespace
{

    constexpr auto                  root_prefix            = "grab-mgr-";
    constexpr auto                  session_name           = "ai";
    constexpr auto                  ghost_name             = "ghost";
    constexpr auto                  provider_failure       = "boom";
    constexpr auto                  start_root_name        = "start";
    constexpr auto                  rollback_root_name     = "rollback";
    constexpr auto                  stop_root_name         = "stop";
    constexpr auto                  unknown_root_name      = "unknown";
    constexpr auto                  expected_destroy_calls = 1U;
    constexpr grab::SessionGeometry default_geometry{};

    [[nodiscard]]
    std::filesystem::path
    fresh_root( std::string_view name )
    {
        auto root = std::filesystem::temp_directory_path() /
                    ( std::string{ root_prefix } + std::string{ name } );
        std::filesystem::remove_all( root );
        return root;
    }

    [[nodiscard]]
    grab::SessionDesc
    desc()
    {
        return grab::SessionDesc{
            .name        = session_name,
            .mode        = grab::SessionMode::Offscreen,
            .geometry    = default_geometry,
            .app_command = {},
        };
    }

}    // namespace

TEST( SessionManager,
      StartWritesReadyRecord )
{
    grab::session::SessionRegistry        registry{ fresh_root( start_root_name ) };
    const grab::test::FakeSessionProvider provider;
    grab::session::SessionManager         manager{ registry, provider };

    const auto                            record = manager.start( desc() );
    ASSERT_TRUE( record.has_value() ) << record.error().message;
    EXPECT_EQ( record->state, grab::SessionState::Ready );
    EXPECT_FALSE( record->endpoint.empty() );
}

TEST( SessionManager,
      StartRollsBackOnProviderFailure )
{
    grab::session::SessionRegistry  registry{ fresh_root( rollback_root_name ) };
    grab::test::FakeSessionProvider provider;
    provider.fail_next_create( grab::ErrorCode::ProviderFailed, provider_failure );
    grab::session::SessionManager manager{ registry, provider };

    const auto                    record = manager.start( desc() );
    ASSERT_FALSE( record.has_value() );
    EXPECT_EQ( record.error().code, grab::ErrorCode::ProviderFailed );
    EXPECT_FALSE( registry.read( session_name ).has_value() );
}

TEST( SessionManager,
      StopRemovesSession )
{
    grab::session::SessionRegistry        registry{ fresh_root( stop_root_name ) };
    const grab::test::FakeSessionProvider provider;
    grab::session::SessionManager         manager{ registry, provider };

    ASSERT_TRUE( manager.start( desc() ).has_value() );
    ASSERT_TRUE( manager.stop( session_name ).has_value() );
    EXPECT_FALSE( registry.read( session_name ).has_value() );
    EXPECT_EQ( provider.destroy_calls(), expected_destroy_calls );
}

TEST( SessionManager,
      StopUnknownIsNotFound )
{
    grab::session::SessionRegistry        registry{ fresh_root( unknown_root_name ) };
    const grab::test::FakeSessionProvider provider;
    grab::session::SessionManager         manager{ registry, provider };

    const auto                            stopped = manager.stop( ghost_name );
    ASSERT_FALSE( stopped.has_value() );
    EXPECT_EQ( stopped.error().code, grab::ErrorCode::SessionNotFound );
}
