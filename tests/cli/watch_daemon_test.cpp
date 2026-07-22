#include "drivers/desktop/x11/config_watch.hpp"
#include "frontends/cli/watch_daemon.hpp"
#include "grab/result.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <nlohmann/json.hpp>    // IWYU pragma: keep
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>
// clang-format on

namespace
{

    constexpr int              successExitCode     = 0;
    constexpr int              failureExitCode     = 1;
    constexpr std::size_t      expectedConfigCount = 1U;
    constexpr std::uint64_t    capturedCount       = 7U;
    constexpr std::uint64_t    errorCount          = 2U;
    constexpr std::uint64_t    skippedCount        = 3U;
    constexpr bool             pausedState         = true;
    constexpr bool             scriptFailedState   = false;
    constexpr std::string_view tempDirectoryPrefix = "grab-watch-daemon-";
    constexpr std::string_view nameSeparator       = "-";
    constexpr std::string_view pidFilename         = "watch.pid";
    constexpr std::string_view statusFilename      = "watch-status.json";
    constexpr std::string_view logFilename         = "watch.log";
    constexpr std::string_view temporarySuffix     = ".tmp";
    constexpr std::string_view configPath          = "/profiles/watch.json";
    constexpr std::string_view lastCapture         = "2026-07-20T12:34:56.789Z";
    constexpr std::string_view staleNeedle         = "stale pid";
    constexpr std::string_view pidField            = "pid";
    constexpr std::string_view liveField           = "live";
    constexpr std::string_view configsField        = "configs";
    constexpr std::string_view configField         = "config";
    constexpr std::string_view capturedField       = "captured";
    constexpr std::string_view errorsField         = "errors";
    constexpr std::string_view skippedField        = "skipped";
    constexpr std::string_view pausedField         = "paused";
    constexpr std::string_view scriptFailedField   = "script_failed";
    constexpr std::string_view lastCaptureField    = "last_capture";

    class TempDaemonDirectory
    {
        public:

            TempDaemonDirectory() :
                path_( std::filesystem::temp_directory_path() / unique_name() )
            {
                std::error_code error;
                static_cast<void>( std::filesystem::remove_all( path_, error ) );
                error.clear();
                static_cast<void>( std::filesystem::create_directories( path_, error ) );
                EXPECT_FALSE( error );
            }

            ~TempDaemonDirectory() noexcept
            {
                std::error_code error;
                static_cast<void>( std::filesystem::remove_all( path_, error ) );
            }

            TempDaemonDirectory( const TempDaemonDirectory& ) = delete;
            TempDaemonDirectory&
            operator=( const TempDaemonDirectory& )      = delete;
            TempDaemonDirectory( TempDaemonDirectory&& ) = delete;
            TempDaemonDirectory&
            operator=( TempDaemonDirectory&& ) = delete;

            [[nodiscard]]
            grab::cli::DaemonPaths
            paths() const
            {
                return grab::cli::DaemonPaths{
                    .pid_file    = path_ / pidFilename,
                    .status_file = path_ / statusFilename,
                    .log_file    = path_ / logFilename,
                };
            }

        private:

            [[nodiscard]]
            static std::string
            unique_name()
            {
                const auto* const test =
                    testing::UnitTest::GetInstance()->current_test_info();
                return std::string{ tempDirectoryPrefix } +
                       test->test_suite_name() +
                       std::string{ nameSeparator } +
                       test->name() +
                       std::string{ nameSeparator } +
                       std::to_string( ::getpid() );
            }

            std::filesystem::path path_;
    };

    void
    write_pid( const std::filesystem::path& path,
               pid_t                        pid )
    {
        std::ofstream output{ path, std::ios::binary | std::ios::trunc };
        ASSERT_TRUE( output.is_open() );
        output << pid << '\n';
        output.close();
        ASSERT_TRUE( output );
    }

}    // namespace

TEST( WatchDaemon,
      StatusRoundTrips )
{
    const TempDaemonDirectory directory;
    const auto                paths    = directory.paths();
    const pid_t               live_pid = ::getpid();
    write_pid( paths.pid_file, live_pid );

    const grab::screen::WatchStats watch_stats{
        .captured      = capturedCount,
        .errors        = errorCount,
        .skipped       = skippedCount,
        .paused        = pausedState,
        .script_failed = scriptFailedState,
        .last_capture  = std::string{ lastCapture },
    };
    const std::vector<std::pair<std::string, grab::screen::WatchStats>> stats{
        { std::string{ configPath }, watch_stats },
    };

    const auto written = grab::cli::write_status( paths, stats );
    ASSERT_TRUE( written.has_value() ) << written.error().message;
    EXPECT_TRUE( std::filesystem::exists( paths.status_file ) );
    EXPECT_FALSE( std::filesystem::exists( std::filesystem::path{
        paths.status_file.string() + std::string{ temporarySuffix }
    } ) );

    testing::internal::CaptureStdout();
    const int         status_exit   = grab::cli::run_watch_status( paths, true );
    const std::string status_output = testing::internal::GetCapturedStdout();
    ASSERT_EQ( status_exit, successExitCode );

    const auto document = nlohmann::json::parse( status_output );
    EXPECT_EQ( document.at( pidField ).get<std::int64_t>(),
               static_cast<std::int64_t>( live_pid ) );
    EXPECT_TRUE( document.at( liveField ).get<bool>() );
    const auto& configs = document.at( configsField );
    ASSERT_EQ( configs.size(), expectedConfigCount );
    const auto& config = configs.front();
    EXPECT_EQ( config.at( configField ).get<std::string>(), configPath );
    EXPECT_EQ( config.at( capturedField ).get<std::uint64_t>(), capturedCount );
    EXPECT_EQ( config.at( errorsField ).get<std::uint64_t>(), errorCount );
    EXPECT_EQ( config.at( skippedField ).get<std::uint64_t>(), skippedCount );
    EXPECT_EQ( config.at( pausedField ).get<bool>(), pausedState );
    EXPECT_EQ( config.at( scriptFailedField ).get<bool>(), scriptFailedState );
    EXPECT_EQ( config.at( lastCaptureField ).get<std::string>(), lastCapture );
}

TEST( WatchDaemon,
      StalePidReported )
{
    constexpr pid_t           knownDeadPid = std::numeric_limits<pid_t>::max();
    const TempDaemonDirectory directory;
    const auto                paths = directory.paths();
    write_pid( paths.pid_file, knownDeadPid );

    testing::internal::CaptureStdout();
    const int         status_exit   = grab::cli::run_watch_status( paths, false );
    const std::string status_output = testing::internal::GetCapturedStdout();

    EXPECT_EQ( status_exit, failureExitCode );
    EXPECT_NE( status_output.find( staleNeedle ), std::string::npos );
    EXPECT_FALSE( std::filesystem::exists( paths.pid_file ) );
}

TEST( WatchDaemon,
      SecondStartWithLivePidFails )
{
    const TempDaemonDirectory directory;
    const auto                paths = directory.paths();
    write_pid( paths.pid_file, ::getpid() );

    const auto result = grab::cli::daemonize( paths );

    ASSERT_FALSE( result.has_value() );
    EXPECT_EQ( result.error().code, grab::ErrorCode::SessionExists );
    EXPECT_TRUE( std::filesystem::exists( paths.pid_file ) );
}
