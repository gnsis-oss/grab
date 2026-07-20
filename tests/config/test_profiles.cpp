#include "grab/config.hpp"
#include "grab/result.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>
// clang-format on

namespace
{

    constexpr std::string_view watchTenSecondProfile = "watch-10s-notify.json";
    constexpr std::string_view watchMinuteProfile    = "watch-1m-notify.json";
    constexpr std::string_view batchExampleProfile   = "batch-example.json";
    constexpr std::string_view homeKey               = "HOME";
    constexpr std::string_view profileHome           = "/tmp/grab-config-profiles-home";
    constexpr std::string_view watchOutputSuffix     = ".cache/grab/watch";
    constexpr std::string_view batchOutputSuffix     = ".cache/grab/batch";
    constexpr std::string_view filenamePattern       = "capture_{timestamp}";
    constexpr std::string_view pngFormat             = "png";
    constexpr std::string_view batchExecutable       = "xmessage";
    constexpr char             environmentSeparator  = '=';
    constexpr std::uint32_t    tenSecondIntervalMs   = 10'000U;
    constexpr std::uint32_t    minuteIntervalMs      = 60'000U;
    constexpr std::uint32_t    popupTimeoutMs        = 2'000U;
    constexpr std::uint32_t    tenSecondAgeDays      = 7U;
    constexpr std::uint32_t    minuteAgeDays         = 30U;
    constexpr std::uint64_t    tenSecondMaxFiles     = 500U;
    constexpr std::uint64_t    minuteMaxFiles        = 1'000U;
    constexpr std::uint64_t    tenSecondDiskMib      = 2'000U;
    constexpr std::uint64_t    minuteDiskMib         = 5'000U;
    constexpr std::uint16_t    batchWidth            = 1'280U;
    constexpr std::uint16_t    batchHeight           = 720U;
    constexpr std::uint8_t     batchDepth            = 24U;
    constexpr std::uint32_t    batchFrames           = 3U;
    constexpr std::uint32_t    batchIntervalMs       = 200U;
    constexpr std::uint32_t    batchDelayMs          = 500U;
    constexpr std::size_t      firstEntry            = 0U;
    constexpr std::size_t      environmentTerminatorCount = 1U;
    constexpr std::size_t      batchTargetCount           = 1U;
    constexpr std::size_t      shippedProfileCount        = 3U;

    constexpr std::array<std::string_view, shippedProfileCount> shippedProfiles{
        watchTenSecondProfile,
        watchMinuteProfile,
        batchExampleProfile,
    };

    class ScopedHome
    {
        public:

            ScopedHome() :
                original_environment_( ::environ )
            {
                for( char* const* entry = original_environment_;
                     entry != nullptr && *entry != nullptr;
                     entry = std::next( entry ) )
                {
                    const std::string_view current{ *entry };
                    const auto separator = current.find( environmentSeparator );
                    if( current.substr( firstEntry, separator ) != homeKey )
                    {
                        entries_.emplace_back( current );
                    }
                }
                std::string home_entry{ homeKey };
                home_entry.push_back( environmentSeparator );
                home_entry.append( profileHome );
                entries_.push_back( std::move( home_entry ) );

                environment_.reserve( entries_.size() + environmentTerminatorCount );
                for( std::string& entry : entries_ )
                {
                    environment_.push_back( entry.data() );
                }
                environment_.push_back( nullptr );
                ::environ = environment_.data();
            }

            ~ScopedHome() noexcept
            {
                ::environ = original_environment_;
            }

            ScopedHome( const ScopedHome& ) = delete;
            ScopedHome&
            operator=( const ScopedHome& ) = delete;
            ScopedHome( ScopedHome&& )     = delete;
            ScopedHome&
            operator=( ScopedHome&& ) = delete;

        private:

            char**                   original_environment_{};
            std::vector<std::string> entries_;
            std::vector<char*>       environment_;
    };

    void
    expect_notifications( const grab::config::NotifySection& notifications )
    {
        EXPECT_TRUE( notifications.enabled );
        EXPECT_EQ( notifications.strategy, grab::config::NotifyStrategy::Os );
        EXPECT_EQ( notifications.popup_timeout_ms, popupTimeoutMs );
    }

    void
    expect_watch_values( const grab::config::WatchSection& watch,
                         std::uint32_t                     interval_ms )
    {
        EXPECT_EQ( watch.interval_ms, interval_ms );
        EXPECT_EQ( watch.output,
                   std::filesystem::path{ profileHome } / watchOutputSuffix );
        EXPECT_EQ( watch.filename, filenamePattern );
        EXPECT_EQ( watch.format, pngFormat );
    }

    void
    expect_watch_limits( const grab::config::WatchLimits& limits,
                         std::uint64_t                    max_files,
                         std::uint32_t                    max_age_days,
                         std::uint64_t                    max_disk_mib )
    {
        EXPECT_EQ( limits.max_files, max_files );
        EXPECT_EQ( limits.max_age_days, max_age_days );
        EXPECT_EQ( limits.max_disk_mib, max_disk_mib );
    }

    void
    expect_watch_profile( const grab::Result<grab::config::Config>& result,
                          std::uint32_t                             interval_ms,
                          std::uint64_t                             max_files,
                          std::uint32_t                             max_age_days,
                          std::uint64_t                             max_disk_mib )
    {
        ASSERT_TRUE( result.has_value() ) << result.error().message;
        ASSERT_TRUE( result->watch.has_value() );
        expect_watch_values( *result->watch, interval_ms );
        expect_watch_limits( result->watch->limits,
                             max_files,
                             max_age_days,
                             max_disk_mib );
        expect_notifications( result->notifications );
    }

    void
    expect_batch_display( const grab::config::DisplaySection& display )
    {
        EXPECT_EQ( display.backend, grab::config::DisplayBackend::Xvfb );
        EXPECT_EQ( display.width, batchWidth );
        EXPECT_EQ( display.height, batchHeight );
        EXPECT_EQ( display.depth, batchDepth );
    }

    void
    expect_batch_target( const std::vector<grab::config::TargetSpec>& targets )
    {
        ASSERT_EQ( targets.size(), batchTargetCount );
        const grab::config::TargetSpec& target = targets.at( firstEntry );
        ASSERT_FALSE( target.argv.empty() );
        EXPECT_EQ( target.argv.at( firstEntry ), batchExecutable );
        EXPECT_EQ( target.frames, batchFrames );
        EXPECT_EQ( target.interval_ms, batchIntervalMs );
        EXPECT_EQ( target.delay_ms, batchDelayMs );
    }

}    // namespace

TEST( ConfigProfiles,
      AllShippedProfilesLoad )
{
    const ScopedHome            home;
    const std::filesystem::path profiles_directory{ GRAB_PROFILES_DIR };

    for( const std::string_view filename : shippedProfiles )
    {
        const auto result = grab::config::load( profiles_directory / filename );
        ASSERT_TRUE( result.has_value() ) << filename << ": " << result.error().message;
    }

    const auto ten_second =
        grab::config::load( profiles_directory / watchTenSecondProfile );
    expect_watch_profile( ten_second,
                          tenSecondIntervalMs,
                          tenSecondMaxFiles,
                          tenSecondAgeDays,
                          tenSecondDiskMib );

    const auto minute = grab::config::load( profiles_directory / watchMinuteProfile );
    expect_watch_profile( minute,
                          minuteIntervalMs,
                          minuteMaxFiles,
                          minuteAgeDays,
                          minuteDiskMib );

    const auto batch = grab::config::load( profiles_directory / batchExampleProfile );
    ASSERT_TRUE( batch.has_value() ) << batch.error().message;
    expect_batch_display( batch->display );
    EXPECT_EQ( batch->batch.output_root,
               std::filesystem::path{ profileHome } / batchOutputSuffix );
    expect_batch_target( batch->targets );
    expect_notifications( batch->notifications );
}
