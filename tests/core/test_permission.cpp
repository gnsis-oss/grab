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

    constexpr std::string_view permission        = "screen.cast";
    constexpr std::string_view token             = "token-contents";
    constexpr std::string_view xdgStateHomeName  = "XDG_STATE_HOME";
    constexpr std::string_view xdgStateHomeValue = "/tmp/xdg-state";
    constexpr std::string_view homeName          = "HOME";
    constexpr std::string_view homeValue         = "/home/tester";
    constexpr std::string_view grabDirName       = "grab";
    constexpr std::string_view localDirName      = ".local";
    constexpr std::string_view stateDirName      = "state";
    constexpr std::string_view testStateDirName  = "grab-test-state";
    constexpr std::string_view restoreTokenName  = "restore.token";

}    // namespace

TEST( Permission,
      NoPermissionBrokerGrantsEverything )
{
    grab::core::NoPermissionBroker broker;
    EXPECT_EQ( broker.query( permission ), grab::core::PermissionState::Granted );
    EXPECT_TRUE( broker.request( permission ).has_value() );
}

TEST( Permission,
      StateDirHonorsXdgStateHome )
{
    const auto dir = grab::core::StateDir::resolve(
        []( std::string_view name ) -> std::optional<std::string>
        {
            if( name == xdgStateHomeName )
            {
                return std::string{ xdgStateHomeValue };
            }
            return std::nullopt;
        }
    );
    EXPECT_EQ( dir,
               std::filesystem::path( std::string{ xdgStateHomeValue } ) /
                   std::string{ grabDirName } );
}

TEST( Permission,
      StateDirFallsBackToHome )
{
    const auto dir = grab::core::StateDir::resolve(
        []( std::string_view name ) -> std::optional<std::string>
        {
            if( name == homeName )
            {
                return std::string{ homeValue };
            }
            return std::nullopt;
        }
    );
    EXPECT_EQ( dir,
               std::filesystem::path( std::string{ homeValue } ) /
                   std::string{ localDirName } /
                   std::string{ stateDirName } /
                   std::string{ grabDirName } );
}

TEST( Permission,
      WriteAtomicCreatesFileWithExactContents )
{
    const auto dir =
        std::filesystem::temp_directory_path() / std::string{ testStateDirName };
    std::filesystem::remove_all( dir );
    const auto file = dir / std::string{ restoreTokenName };

    ASSERT_TRUE( grab::core::StateDir::write_atomic( file, token ).has_value() );

    std::ifstream     input( file );
    const std::string contents{
        std::istreambuf_iterator<char>{ input },
        std::istreambuf_iterator<char>{}
    };
    EXPECT_EQ( contents, token );
    std::filesystem::remove_all( dir );
}
