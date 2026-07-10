#include "grab/result.hpp"
#include "grab/session.hpp"
#include "session/record.hpp"
#include "session/registry.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <utility>
// clang-format on

namespace
{

    grab::session::SessionRecord
    rec( std::string name )
    {
        grab::session::SessionRecord r;
        r.name     = std::move( name );
        r.provider = "fake";
        r.state    = grab::SessionState::Starting;
        return r;
    }

    std::filesystem::path
    temp_root( const testing::TestInfo* info )
    {
        std::string name  = "grab-reg-";
        name             += info->name();
        return std::filesystem::temp_directory_path() / name;
    }

}    // namespace

TEST( SessionRegistry,
      CreateThenReadRoundTrips )
{
    const auto root = temp_root( testing::UnitTest::GetInstance()->current_test_info() );
    std::filesystem::remove_all( root );
    grab::session::SessionRegistry registry{ root };

    ASSERT_TRUE( registry.create( rec( "ai" ) ).has_value() );
    const auto read = registry.read( "ai" );
    ASSERT_TRUE( read.has_value() ) << read.error().message;
    EXPECT_EQ( read->name, "ai" );
}

TEST( SessionRegistry,
      CreateRejectsDuplicateName )
{
    const auto root = temp_root( testing::UnitTest::GetInstance()->current_test_info() );
    std::filesystem::remove_all( root );
    grab::session::SessionRegistry registry{ root };

    ASSERT_TRUE( registry.create( rec( "ai" ) ).has_value() );
    const auto second = registry.create( rec( "ai" ) );
    ASSERT_FALSE( second.has_value() );
    EXPECT_EQ( second.error().code, grab::ErrorCode::SessionExists );
}

TEST( SessionRegistry,
      ReadMissingIsNotFound )
{
    const auto root = temp_root( testing::UnitTest::GetInstance()->current_test_info() );
    std::filesystem::remove_all( root );
    grab::session::SessionRegistry registry{ root };

    const auto                     read = registry.read( "ghost" );
    ASSERT_FALSE( read.has_value() );
    EXPECT_EQ( read.error().code, grab::ErrorCode::SessionNotFound );
}

TEST( SessionRegistryLiveness,
      HeldLockReadsAsLiveThenDeadAfterRelease )
{
    const auto root = temp_root( testing::UnitTest::GetInstance()->current_test_info() );
    std::filesystem::remove_all( root );
    grab::session::SessionRegistry registry{ root };
    ASSERT_TRUE( registry.create( rec( "ai" ) ).has_value() );

    EXPECT_FALSE( registry.is_live( "ai" ) );

    const auto lock = registry.acquire_liveness_lock( "ai" );
    ASSERT_TRUE( lock.has_value() ) << lock.error().message;
    EXPECT_TRUE( registry.is_live( "ai" ) );

    ::close( lock.value() );
    EXPECT_FALSE( registry.is_live( "ai" ) );
}

TEST( SessionRegistryLiveness,
      ReapRemovesDeadKeepsLive )
{
    const auto root = temp_root( testing::UnitTest::GetInstance()->current_test_info() );
    std::filesystem::remove_all( root );
    grab::session::SessionRegistry registry{ root };
    ASSERT_TRUE( registry.create( rec( "dead" ) ).has_value() );
    ASSERT_TRUE( registry.create( rec( "live" ) ).has_value() );

    const auto lock = registry.acquire_liveness_lock( "live" );
    ASSERT_TRUE( lock.has_value() );

    EXPECT_EQ( registry.reap_dead(), 1U );
    EXPECT_FALSE( registry.read( "dead" ).has_value() );
    EXPECT_TRUE( registry.read( "live" ).has_value() );
    ::close( lock.value() );
}
