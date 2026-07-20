#include "codec/png.hpp"
#include "config/batch_manifest.hpp"
#include "drivers/desktop/x11/config_batch.hpp"
#include "grab/config.hpp"
#include "grab/image.hpp"
#include "grab/result.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
// clang-format on

namespace
{

    constexpr std::uint16_t    displayWidth          = 640U;
    constexpr std::uint16_t    displayHeight         = 480U;
    constexpr std::uint8_t     displayDepth          = 24U;
    constexpr std::uint32_t    helperWindowWidth     = 240U;
    constexpr std::uint32_t    helperWindowHeight    = 160U;
    constexpr std::uint32_t    rgbaChannelCount      = 4U;
    constexpr std::uint8_t     referenceRed          = 220U;
    constexpr std::uint8_t     referenceGreen        = 30U;
    constexpr std::uint8_t     referenceBlue         = 40U;
    constexpr std::uint8_t     opaqueAlpha           = 255U;
    constexpr std::uint32_t    happyFrameCount       = 2U;
    constexpr std::uint32_t    singleFrameCount      = 1U;
    constexpr std::uint32_t    frameIntervalMs       = 50U;
    constexpr std::uint32_t    settleDelayMs         = 50U;
    constexpr std::uint32_t    noSettleDelayMs       = 0U;
    constexpr double           targetTimeoutSeconds  = 30.0;
    constexpr double           exactThreshold        = 0.0;
    constexpr std::int64_t     minimumChildPid       = 1;
    constexpr std::uint32_t    noWindowId            = 0U;
    constexpr std::uint32_t    noFailures            = 0U;
    constexpr std::uint32_t    oneFailure            = 1U;
    constexpr std::uint32_t    twoFailures           = 2U;
    constexpr std::size_t      oneTarget             = 1U;
    constexpr std::size_t      twoFiles              = 2U;
    constexpr std::size_t      twoCompareEntries     = 2U;
    constexpr std::size_t      firstFileIndex        = 0U;
    constexpr std::size_t      secondFileIndex       = 1U;
    constexpr std::size_t      firstPixelIndex       = 0U;
    constexpr std::uintmax_t   nonemptyFileSize      = 0U;
    constexpr std::string_view tempDirectoryPrefix   = "grab-config-batch-";
    constexpr std::string_view nameSeparator         = "-";
    constexpr std::string_view profileFilename       = "batch-profile.json";
    constexpr std::string_view sessionsDirectoryName = "sessions";
    constexpr std::string_view currentDirectoryName  = "current";
#ifndef GRAB_CONFIG_BATCH_WINDOW_PATH
    constexpr std::string_view helperExecutableName  = "grab_config_batch_window";
    constexpr std::string_view processExecutableLink = "/proc/self/exe";
#endif
    constexpr std::string_view helperWindowClass      = "GrabConfigBatchWindow";
    constexpr std::string_view happyTargetName        = "happy";
    constexpr std::string_view happyFirstFrame        = "happy.png";
    constexpr std::string_view happySecondFrame       = "happy_002.png";
    constexpr std::string_view falseTargetName        = "false-target";
    constexpr std::string_view falseExecutable        = "/bin/false";
    constexpr std::string_view compareTargetName      = "compare-window";
    constexpr std::string_view compareFrame           = "compare-window.png";
    constexpr std::string_view missingFrame           = "missing.png";
    constexpr std::string_view referenceDirectoryName = "reference";

    [[nodiscard]]
    std::byte
    byte_from( std::uint8_t value ) noexcept
    {
        return static_cast<std::byte>( value );
    }

    class TempDirectory
    {
        public:

            TempDirectory() :
                path_( std::filesystem::temp_directory_path() / unique_name() )
            {
                std::error_code error;
                static_cast<void>( std::filesystem::remove_all( path_, error ) );
                error.clear();
                const bool created = std::filesystem::create_directories( path_, error );
                EXPECT_TRUE( created );
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
    std::filesystem::path
    helper_executable()
    {
#ifdef GRAB_CONFIG_BATCH_WINDOW_PATH
        return std::filesystem::path{ GRAB_CONFIG_BATCH_WINDOW_PATH };
#else
        std::error_code error;
        const auto      test_executable =
            std::filesystem::read_symlink( processExecutableLink, error );
        EXPECT_FALSE( error ) << error.message();
        return test_executable.parent_path() / helperExecutableName;
#endif
    }

    [[nodiscard]]
    grab::config::TargetSpec
    helper_target( std::string_view name,
                   std::uint32_t    frames )
    {
        grab::config::TargetSpec target;
        target.name        = name;
        target.argv        = { helper_executable().string() };
        target.match       = grab::config::MatchKind::WmClass;
        target.pattern     = helperWindowClass;
        target.frames      = frames;
        target.interval_ms = frameIntervalMs;
        target.delay_ms    = settleDelayMs;
        target.timeout_s   = targetTimeoutSeconds;
        target.kill_after  = true;
        return target;
    }

    [[nodiscard]]
    grab::config::Config
    base_config( const TempDirectory& temp )
    {
        grab::config::Config config;
        config.source                = temp.path() / profileFilename;
        config.display.backend       = grab::config::DisplayBackend::Xvfb;
        config.display.width         = displayWidth;
        config.display.height        = displayHeight;
        config.display.depth         = displayDepth;
        config.batch.output_root     = temp.path() / sessionsDirectoryName;
        config.notifications.enabled = false;
        return config;
    }

    [[nodiscard]]
    std::filesystem::path
    current_directory( const grab::screen::ConfigBatchResult& result )
    {
        return result.session_dir / currentDirectoryName;
    }

    [[nodiscard]]
    grab::Image
    reference_image()
    {
        const auto stride      = helperWindowWidth * rgbaChannelCount;
        const auto pixel_count = static_cast<std::size_t>( helperWindowWidth ) *
                                 static_cast<std::size_t>( helperWindowHeight );
        std::vector<std::byte> pixels;
        pixels.reserve( pixel_count * static_cast<std::size_t>( rgbaChannelCount ) );
        for( std::size_t pixel = firstPixelIndex; pixel < pixel_count; ++pixel )
        {
            pixels.push_back( byte_from( referenceRed ) );
            pixels.push_back( byte_from( referenceGreen ) );
            pixels.push_back( byte_from( referenceBlue ) );
            pixels.push_back( byte_from( opaqueAlpha ) );
        }
        return grab::Image{
            .width  = helperWindowWidth,
            .height = helperWindowHeight,
            .stride = stride,
            .format = grab::PixelFormat::Rgba,
            .pixels = std::move( pixels ),
        };
    }

    [[nodiscard]]
    grab::Result<void>
    write_png( const std::filesystem::path& path,
               const grab::Image&           image )
    {
        auto encoded = grab::codec::encode_png( image );
        if( !encoded.has_value() )
        {
            return std::unexpected( std::move( encoded.error() ) );
        }

        std::ofstream stream{ path, std::ios::binary | std::ios::trunc };
        if( !stream )
        {
            return grab::fail( grab::ErrorCode::ProviderFailed,
                               "failed to open reference PNG: " + path.string() );
        }
        for( const std::byte value : *encoded )
        {
            stream.put( static_cast<char>( std::to_integer<unsigned char>( value ) ) );
        }
        if( !stream )
        {
            return grab::fail( grab::ErrorCode::ProviderFailed,
                               "failed to write reference PNG: " + path.string() );
        }
        return {};
    }

    [[nodiscard]]
    const grab::config::FileCompareEntry*
    find_compare_entry( const grab::config::BatchManifest& manifest,
                        std::string_view                   name )
    {
        const auto entry =
            std::ranges::find_if( manifest.compare,
                                  [name]( const grab::config::FileCompareEntry& value )
                                  {
                                      return value.name == name;
                                  } );
        return entry == manifest.compare.end() ? nullptr : &*entry;
    }

    void
    expect_nonempty_regular_file( const std::filesystem::path& path )
    {
        std::error_code error;
        const bool      regular = std::filesystem::is_regular_file( path, error );
        ASSERT_FALSE( error ) << error.message();
        ASSERT_TRUE( regular ) << path;
        const auto size = std::filesystem::file_size( path, error );
        ASSERT_FALSE( error ) << error.message();
        EXPECT_GT( size, nonemptyFileSize );
    }

}    // namespace

TEST( ConfigBatch,
      CapturesTwoFramesAndWritesDoneManifest )
{
    const TempDirectory temp;
    auto                config = base_config( temp );
    config.targets.push_back( helper_target( happyTargetName, happyFrameCount ) );

    auto result = grab::screen::run_config_batch( config, nullptr );

    ASSERT_TRUE( result.has_value() ) << result.error().message;
    EXPECT_EQ( result->target_errors, noFailures );
    EXPECT_EQ( result->compare_failures, noFailures );
    EXPECT_EQ( result->manifest.state, grab::config::RunState::Done );
    EXPECT_EQ( result->manifest.profile, config.source );
    EXPECT_FALSE( result->manifest.started_at.empty() );
    EXPECT_FALSE( result->manifest.ended_at.empty() );
    EXPECT_TRUE( std::filesystem::is_directory( current_directory( *result ) ) );

    ASSERT_EQ( result->manifest.targets.size(), oneTarget );
    const auto& outcome = result->manifest.targets.front();
    EXPECT_EQ( outcome.name, happyTargetName );
    EXPECT_GT( outcome.pid, minimumChildPid );
    EXPECT_NE( outcome.window_id, noWindowId );
    EXPECT_TRUE( outcome.error.empty() );
    ASSERT_EQ( outcome.files.size(), twoFiles );
    EXPECT_EQ( outcome.files.at( firstFileIndex ), happyFirstFrame );
    EXPECT_EQ( outcome.files.at( secondFileIndex ), happySecondFrame );

    const auto first_path  = current_directory( *result ) / happyFirstFrame;
    const auto second_path = current_directory( *result ) / happySecondFrame;
    expect_nonempty_regular_file( first_path );
    expect_nonempty_regular_file( second_path );

    auto persisted = grab::config::BatchManifest::read( result->session_dir );
    ASSERT_TRUE( persisted.has_value() ) << persisted.error().message;
    EXPECT_EQ( persisted->state, grab::config::RunState::Done );
    EXPECT_FALSE( persisted->ended_at.empty() );
    ASSERT_EQ( persisted->targets.size(), oneTarget );
    EXPECT_EQ( persisted->targets.front().files, outcome.files );
}

TEST( ConfigBatch,
      EarlyChildExitRecordsFailedManifest )
{
    const TempDirectory      temp;
    auto                     config = base_config( temp );
    grab::config::TargetSpec target;
    target.name        = falseTargetName;
    target.argv        = { std::string{ falseExecutable } };
    target.match       = grab::config::MatchKind::Pid;
    target.frames      = singleFrameCount;
    target.interval_ms = frameIntervalMs;
    target.delay_ms    = noSettleDelayMs;
    target.timeout_s   = targetTimeoutSeconds;
    target.kill_after  = true;
    config.targets.push_back( std::move( target ) );

    auto result = grab::screen::run_config_batch( config, nullptr );

    ASSERT_TRUE( result.has_value() ) << result.error().message;
    EXPECT_EQ( result->target_errors, oneFailure );
    EXPECT_EQ( result->compare_failures, noFailures );
    EXPECT_EQ( result->manifest.state, grab::config::RunState::Failed );
    EXPECT_FALSE( result->manifest.ended_at.empty() );
    ASSERT_EQ( result->manifest.targets.size(), oneTarget );
    const auto& outcome = result->manifest.targets.front();
    EXPECT_EQ( outcome.name, falseTargetName );
    EXPECT_GT( outcome.pid, minimumChildPid );
    EXPECT_EQ( outcome.window_id, noWindowId );
    EXPECT_TRUE( outcome.files.empty() );
    EXPECT_FALSE( outcome.error.empty() );

    auto persisted = grab::config::BatchManifest::read( result->session_dir );
    ASSERT_TRUE( persisted.has_value() ) << persisted.error().message;
    EXPECT_EQ( persisted->state, grab::config::RunState::Failed );
    EXPECT_FALSE( persisted->ended_at.empty() );
    ASSERT_EQ( persisted->targets.size(), oneTarget );
    EXPECT_FALSE( persisted->targets.front().error.empty() );
}

TEST( ConfigBatch,
      CompareRecordsMismatchAndMissingFailures )
{
    const TempDirectory temp;
    auto                config = base_config( temp );
    config.targets.push_back( helper_target( compareTargetName, singleFrameCount ) );
    config.compare.mode       = grab::config::CompareMode::Exact;
    config.compare.threshold  = exactThreshold;

    const auto      reference = temp.path() / referenceDirectoryName;
    std::error_code reference_error;
    ASSERT_TRUE( std::filesystem::create_directory( reference, reference_error ) );
    ASSERT_FALSE( reference_error ) << reference_error.message();
    const grab::Image image            = reference_image();
    auto              mismatch_written = write_png( reference / compareFrame, image );
    ASSERT_TRUE( mismatch_written.has_value() ) << mismatch_written.error().message;
    auto missing_written = write_png( reference / missingFrame, image );
    ASSERT_TRUE( missing_written.has_value() ) << missing_written.error().message;
    config.compare.ref = reference;

    auto result        = grab::screen::run_config_batch( config, nullptr );

    ASSERT_TRUE( result.has_value() ) << result.error().message;
    EXPECT_EQ( result->target_errors, noFailures );
    EXPECT_EQ( result->compare_failures, twoFailures );
    EXPECT_EQ( result->manifest.state, grab::config::RunState::Failed );
    EXPECT_FALSE( result->manifest.ended_at.empty() );
    ASSERT_EQ( result->manifest.compare.size(), twoCompareEntries );

    const auto* mismatch = find_compare_entry( result->manifest, compareFrame );
    ASSERT_NE( mismatch, nullptr );
    EXPECT_FALSE( mismatch->passed );
    EXPECT_GT( mismatch->score, exactThreshold );
    const auto* missing = find_compare_entry( result->manifest, missingFrame );
    ASSERT_NE( missing, nullptr );
    EXPECT_FALSE( missing->passed );
    EXPECT_DOUBLE_EQ( missing->score, exactThreshold );

    auto persisted = grab::config::BatchManifest::read( result->session_dir );
    ASSERT_TRUE( persisted.has_value() ) << persisted.error().message;
    EXPECT_EQ( persisted->state, grab::config::RunState::Failed );
    EXPECT_FALSE( persisted->ended_at.empty() );
    ASSERT_EQ( persisted->compare.size(), twoCompareEntries );
    const auto* persisted_mismatch = find_compare_entry( *persisted, compareFrame );
    ASSERT_NE( persisted_mismatch, nullptr );
    EXPECT_FALSE( persisted_mismatch->passed );
    const auto* persisted_missing = find_compare_entry( *persisted, missingFrame );
    ASSERT_NE( persisted_missing, nullptr );
    EXPECT_FALSE( persisted_missing->passed );
}
