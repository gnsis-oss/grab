#include "config/pattern.hpp"
#include "drivers/desktop/x11/config_watch.hpp"
#include "grab/config.hpp"
#include "grab/result.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>
#include <xcb/xcb.h>
// clang-format on

namespace
{

    constexpr const char*      xvfbDisplay                = ":89";
    constexpr int              xcbSuccess                 = 0;
    constexpr int              initialScreenIndex         = 0;
    constexpr std::size_t      environmentTerminatorCount = 1U;
    constexpr std::size_t      minimumCaptureCount        = 5U;
    constexpr std::uint32_t    captureIntervalMs          = 100U;
    constexpr std::uint32_t    scriptDelayMs              = 50U;
    constexpr std::uint64_t    unlimitedFiles             = 0U;
    constexpr std::uint64_t    rotationMaximumFiles       = 3U;
    constexpr std::int16_t     firstMoveX                 = 120;
    constexpr std::int16_t     firstMoveY                 = 140;
    constexpr std::int16_t     secondMoveX                = 240;
    constexpr std::int16_t     secondMoveY                = 260;
    constexpr auto             capturePollInterval   = std::chrono::milliseconds{ 50 };
    constexpr auto             captureWaitLimit      = std::chrono::seconds{ 30 };
    constexpr std::string_view displayEnvironmentKey = "DISPLAY";
    constexpr std::string_view tempDirectoryPrefix   = "grab-config-watch-";
    constexpr std::string_view nameSeparator         = "-";
    constexpr std::string_view capturePattern        = "capture_{timestamp}_{seq}";

    class ScopedEnvironment
    {
        public:

            ScopedEnvironment( std::string_view                name,
                               std::optional<std::string_view> value ) :
                original_environment_( ::environ )
            {
                for( char* const* entry = original_environment_;
                     entry != nullptr && *entry != nullptr;
                     entry = std::next( entry ) )
                {
                    const std::string_view current{ *entry };
                    const auto             separator = current.find( '=' );
                    if( current.substr( 0U, separator ) != name )
                    {
                        entries_.emplace_back( current );
                    }
                }
                if( value.has_value() )
                {
                    entries_.emplace_back(
                        std::string{ name } + "=" + std::string{ *value }
                    );
                }

                environment_.reserve( entries_.size() + environmentTerminatorCount );
                for( std::string& entry : entries_ )
                {
                    environment_.push_back( entry.data() );
                }
                environment_.push_back( nullptr );
                ::environ = environment_.data();
            }

            ~ScopedEnvironment()
            {
                ::environ = original_environment_;
            }

            ScopedEnvironment( const ScopedEnvironment& ) = delete;
            ScopedEnvironment&
            operator=( const ScopedEnvironment& )    = delete;
            ScopedEnvironment( ScopedEnvironment&& ) = delete;
            ScopedEnvironment&
            operator=( ScopedEnvironment&& ) = delete;

        private:

            char**                   original_environment_ = nullptr;
            std::vector<std::string> entries_;
            std::vector<char*>       environment_;
    };

    class TestConnection
    {
        public:

            explicit TestConnection( const char* display ) :
                connection_( xcb_connect( display,
                                          &screen_index_ ) )
            {
            }

            ~TestConnection()
            {
                if( connection_ != nullptr )
                {
                    xcb_disconnect( connection_ );
                }
            }

            TestConnection( const TestConnection& ) = delete;
            TestConnection&
            operator=( const TestConnection& ) = delete;
            TestConnection( TestConnection&& ) = delete;
            TestConnection&
            operator=( TestConnection&& ) = delete;

            [[nodiscard]]
            xcb_connection_t*
            get() const noexcept
            {
                return connection_;
            }

        private:

            xcb_connection_t* connection_   = nullptr;
            int               screen_index_ = initialScreenIndex;
    };

    class TempDirectory
    {
        public:

            TempDirectory() :
                path_( std::filesystem::temp_directory_path() / unique_name() )
            {
                std::error_code error;
                static_cast<void>( std::filesystem::remove_all( path_, error ) );
                error.clear();
                static_cast<void>( std::filesystem::create_directories( path_, error ) );
                EXPECT_FALSE( error ) << error.message();
            }

            ~TempDirectory() noexcept
            {
                std::error_code error;
                static_cast<void>( std::filesystem::remove_all( path_, error ) );
            }

            TempDirectory( const TempDirectory& ) = delete;
            TempDirectory&
            operator=( const TempDirectory& ) = delete;
            TempDirectory( TempDirectory&& )  = delete;
            TempDirectory&
            operator=( TempDirectory&& ) = delete;

            [[nodiscard]]
            const std::filesystem::path&
            path() const noexcept
            {
                return path_;
            }

        private:

            [[nodiscard]]
            static std::string
            unique_name()
            {
                const auto* info = testing::UnitTest::GetInstance()->current_test_info();
                return std::string{ tempDirectoryPrefix } +
                       info->test_suite_name() +
                       std::string{ nameSeparator } +
                       info->name();
            }

            std::filesystem::path path_;
    };

    [[nodiscard]]
    grab::config::Config
    make_config( const std::filesystem::path& output,
                 std::uint64_t                max_files )
    {
        grab::config::Config config;
        config.display.backend = grab::config::DisplayBackend::Native;

        grab::config::WatchSection watch;
        watch.interval_ms      = captureIntervalMs;
        watch.output           = output;
        watch.filename         = std::string{ capturePattern };
        watch.limits.max_files = max_files;
        config.watch           = std::move( watch );

        grab::config::ScriptStep first_move;
        first_move.action = grab::config::StepAction::Move;
        first_move.x      = firstMoveX;
        first_move.y      = firstMoveY;

        grab::config::ScriptStep second_move;
        second_move.action = grab::config::StepAction::Move;
        second_move.x      = secondMoveX;
        second_move.y      = secondMoveY;

        grab::config::ScriptStep delay;
        delay.action   = grab::config::StepAction::Delay;
        delay.delay_ms = scriptDelayMs;

        grab::config::ScriptSection script;
        script.loop = true;
        script.steps =
            { std::move( first_move ), std::move( second_move ), std::move( delay ) };
        config.script = std::move( script );
        return config;
    }

    [[nodiscard]]
    std::vector<std::filesystem::path>
    capture_files( const std::filesystem::path& output )
    {
        std::vector<std::filesystem::path> files;
        for( const auto& entry : std::filesystem::directory_iterator{ output } )
        {
            if( entry.is_regular_file() )
            {
                files.push_back( entry.path() );
            }
        }
        return files;
    }

    void
    expect_files_match_pattern( const std::vector<std::filesystem::path>& files )
    {
        for( const auto& file : files )
        {
            EXPECT_TRUE(
                grab::config::matches_pattern( capturePattern,
                                               file.filename().generic_string() )
            ) << file;
        }
    }

    // Sanitizers and coverage make capture latency unpredictable: wait for a
    // capture count instead of sleeping a fixed duration.
    [[nodiscard]]
    bool
    wait_for_captures( const grab::screen::ConfigWatcher& watcher,
                       std::uint64_t                      count )
    {
        const auto deadline = std::chrono::steady_clock::now() + captureWaitLimit;
        while( std::chrono::steady_clock::now() < deadline )
        {
            if( watcher.stats().captured >= count )
            {
                return true;
            }
            std::this_thread::sleep_for( capturePollInterval );
        }
        return false;
    }

    class ConfigWatch : public testing::Test
    {
        protected:

            void
            SetUp() override
            {
                ASSERT_NE( connection_.get(), nullptr );
                ASSERT_EQ( xcb_connection_has_error( connection_.get() ), xcbSuccess );
            }

        private:

            ScopedEnvironment environment_{
                displayEnvironmentKey,
                std::string_view{ xvfbDisplay },
            };
            TestConnection connection_{ xvfbDisplay };
    };

}    // namespace

TEST_F( ConfigWatch,
        CapturesOnScheduleAndRunsLoopingScript )
{
    const TempDirectory        temp;
    const grab::config::Config config  = make_config( temp.path(), unlimitedFiles );
    auto                       watcher = grab::screen::ConfigWatcher::start( config );
    ASSERT_TRUE( watcher.has_value() ) << watcher.error().message;

    EXPECT_TRUE( wait_for_captures( *watcher, minimumCaptureCount ) );
    watcher->stop();

    const grab::screen::WatchStats stats = watcher->stats();
    const auto                     files = capture_files( temp.path() );
    ASSERT_GE( files.size(), minimumCaptureCount );
    expect_files_match_pattern( files );
    EXPECT_EQ( stats.captured, static_cast<std::uint64_t>( files.size() ) );
    EXPECT_FALSE( stats.script_failed );
}

TEST_F( ConfigWatch,
        MaxFilesRotationHoldsAtLimit )
{
    const TempDirectory        temp;
    const grab::config::Config config = make_config( temp.path(), rotationMaximumFiles );
    auto                       watcher = grab::screen::ConfigWatcher::start( config );
    ASSERT_TRUE( watcher.has_value() ) << watcher.error().message;

    EXPECT_TRUE( wait_for_captures( *watcher, rotationMaximumFiles + 1U ) );
    watcher->stop();

    const grab::screen::WatchStats stats = watcher->stats();
    const auto                     files = capture_files( temp.path() );
    expect_files_match_pattern( files );
    EXPECT_GT( stats.captured, rotationMaximumFiles );
    EXPECT_EQ( files.size(), static_cast<std::size_t>( rotationMaximumFiles ) );
    EXPECT_FALSE( stats.script_failed );
}
