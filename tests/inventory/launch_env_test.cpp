#include "inventory/process.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <utility>
#include <vector>
// clang-format on

namespace
{

    constexpr std::string_view shell         = "/bin/sh";
    constexpr std::string_view marker_token  = "grab-ok";
    constexpr auto             poll_step     = std::chrono::milliseconds( 20 );
    constexpr int              poll_attempts = 200;    // ~4s budget

    [[nodiscard]]
    std::filesystem::path
    marker_path( const testing::TestInfo* info )
    {
        return std::filesystem::temp_directory_path() /
               ( std::string( "grab-env-" ) + info->name() );
    }

    [[nodiscard]]
    bool
    wait_for_file( const std::filesystem::path& path )
    {
        for( int attempt = 0; attempt < poll_attempts; ++attempt )
        {
            if( std::filesystem::exists( path ) )
            {
                return true;
            }
            std::this_thread::sleep_for( poll_step );
        }
        return false;
    }

}    // namespace

TEST( LaunchEnvInjection,
      ChildSeesInjectedVariable )
{
    const auto marker =
        marker_path( testing::UnitTest::GetInstance()->current_test_info() );
    std::filesystem::remove( marker );

    // Child touches the marker only if GRAB_TEST_ENV holds the injected token.
    const std::string                 script = std::string( "[ \"$GRAB_TEST_ENV\" = " ) +
                                               std::string( marker_token ) +
                                               " ] && : > " +
                                               marker.string();
    const std::array<std::string, 2U> args{ "-c", script };
    const std::vector<std::pair<std::string, std::string>> env{
        { "GRAB_TEST_ENV", std::string( marker_token ) },
    };

    const auto pid = grab::inventory::launch_app( shell, args, env );
    ASSERT_TRUE( pid.has_value() ) << pid.error().message;

    EXPECT_TRUE( wait_for_file( marker ) );
    int status = 0;
    ::waitpid( pid.value(), &status, 0 );    // reap
    std::filesystem::remove( marker );
}
