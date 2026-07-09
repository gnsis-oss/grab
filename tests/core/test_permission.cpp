#include "core/permission.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
// clang-format on

namespace
{

    constexpr std::string_view kPermission        = "screen.cast";
    constexpr std::string_view kToken             = "token-contents";
    constexpr std::string_view kXdgStateHomeName  = "XDG_STATE_HOME";
    constexpr std::string_view kXdgStateHomeValue = "/tmp/xdg-state";
    constexpr std::string_view kHomeName          = "HOME";
    constexpr std::string_view kHomeValue         = "/home/tester";
    constexpr std::string_view kGrabDirName       = "grab";
    constexpr std::string_view kLocalDirName      = ".local";
    constexpr std::string_view kStateDirName      = "state";
    constexpr std::string_view kTestStateDirName  = "grab-test-state";
    constexpr std::string_view kRestoreTokenName  = "restore.token";

}    // namespace

TEST( Permission,
      NoPermissionBrokerGrantsEverything )
{
    grab::core::NoPermissionBroker broker;
    EXPECT_EQ( broker.query( kPermission ), grab::core::PermissionState::granted );
    EXPECT_TRUE( broker.request( kPermission ).has_value() );
}

TEST( Permission,
      StateDirHonorsXdgStateHome )
{
    const auto dir = grab::core::StateDir::resolve(
        []( std::string_view name ) -> std::optional<std::string>
        {
            if( name == kXdgStateHomeName )
            {
                return std::string{ kXdgStateHomeValue };
            }
            return std::nullopt;
        }
    );
    EXPECT_EQ( dir,
               std::filesystem::path( std::string{ kXdgStateHomeValue } ) /
                   std::string{ kGrabDirName } );
}

TEST( Permission,
      StateDirFallsBackToHome )
{
    const auto dir = grab::core::StateDir::resolve(
        []( std::string_view name ) -> std::optional<std::string>
        {
            if( name == kHomeName )
            {
                return std::string{ kHomeValue };
            }
            return std::nullopt;
        }
    );
    EXPECT_EQ( dir,
               std::filesystem::path( std::string{ kHomeValue } ) /
                   std::string{ kLocalDirName } /
                   std::string{ kStateDirName } /
                   std::string{ kGrabDirName } );
}

TEST( Permission,
      WriteAtomicCreatesFileWithExactContents )
{
    const auto dir =
        std::filesystem::temp_directory_path() / std::string{ kTestStateDirName };
    std::filesystem::remove_all( dir );
    const auto file = dir / std::string{ kRestoreTokenName };

    ASSERT_TRUE( grab::core::StateDir::write_atomic( file, kToken ).has_value() );

    std::ifstream     input( file );
    const std::string contents{
        std::istreambuf_iterator<char>{ input },
        std::istreambuf_iterator<char>{}
    };
    EXPECT_EQ( contents, kToken );
    std::filesystem::remove_all( dir );
}
