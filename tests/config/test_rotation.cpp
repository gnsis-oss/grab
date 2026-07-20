#include "config/rotation.hpp"
#include "grab/config.hpp"
#include "grab/result.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
// clang-format on

namespace
{

    constexpr std::string_view tempPrefix           = "grab-config-rotation-";
    constexpr std::string_view nameSeparator        = "-";
    constexpr std::string_view pattern              = "capture_{seq}";
    constexpr std::string_view firstCaptureName     = "capture_00001.png";
    constexpr std::string_view secondCaptureName    = "capture_00002.png";
    constexpr std::string_view thirdCaptureName     = "capture_00003.png";
    constexpr std::string_view foreignName          = "unrelated.txt";
    constexpr std::string_view ledgerDirectoryName  = "captures";
    constexpr std::string_view nestedDirectoryName  = "nested";
    constexpr std::string_view nestedPattern        = "nested/capture_{seq}";
    constexpr std::string_view symlinkName          = "capture_00004.png";
    constexpr std::string_view symlinkTargetName    = "outside-target.png";
    constexpr std::string_view danglingSymlinkName  = "capture_00005.png";
    constexpr std::string_view missingTargetName    = "missing-target.png";
    constexpr std::string_view directorySymlinkName = "nested-link";
    constexpr std::string_view missingDirectoryName = "missing-directory";
    constexpr std::string_view linkedDirectoryPattern =
        "nested-link/missing-child/capture_{seq}";
    constexpr char           fillerByte            = 'x';
    constexpr std::uintmax_t zeroBytes             = 0U;
    constexpr std::uintmax_t oneByte               = 1U;
    constexpr std::uintmax_t bytesPerKibibyte      = 1'024U;
    constexpr std::uintmax_t bytesPerMebibyte      = bytesPerKibibyte * bytesPerKibibyte;
    constexpr std::uintmax_t overDiskCapBytes      = bytesPerMebibyte + oneByte;
    constexpr std::uintmax_t hysteresisNumerator   = 9U;
    constexpr std::uintmax_t hysteresisDenominator = 10U;
    constexpr std::uintmax_t belowResumeThreshold =
        ( bytesPerMebibyte * hysteresisNumerator ) / hysteresisDenominator;
    constexpr std::uintmax_t atResumeThreshold = belowResumeThreshold + oneByte;
    constexpr std::uint64_t  unlimitedFiles    = 0U;
    constexpr std::uint64_t  oneFile           = 1U;
    constexpr std::uint64_t  twoFiles          = 2U;
    constexpr std::uint64_t  oneMebibyte       = 1U;
    constexpr std::uint32_t  unlimitedAgeDays  = 0U;
    constexpr std::uint32_t  twoAgeDays        = 2U;
    constexpr std::uint32_t  maximumAgeDays  = std::numeric_limits<std::uint32_t>::max();
    constexpr std::size_t    noPrunedFiles   = 0U;
    constexpr std::size_t    onePrunedFile   = 1U;
    constexpr int            hoursPerDay     = 24;
    constexpr int            veryOldDayCount = 10;
    constexpr int            oldestDayCount  = 5;
    constexpr int            expiredDayCount = 3;
    constexpr int            recentDayCount  = 1;
    constexpr std::chrono::hours veryOldAge{ veryOldDayCount * hoursPerDay };
    constexpr std::chrono::hours oldestAge{ oldestDayCount * hoursPerDay };
    constexpr std::chrono::hours expiredAge{ expiredDayCount * hoursPerDay };
    constexpr std::chrono::hours recentAge{ recentDayCount * hoursPerDay };

    class TempRotation
    {
        public:

            TempRotation() :
                path_( std::filesystem::temp_directory_path() / unique_name() )
            {
                std::error_code error;
                static_cast<void>( std::filesystem::remove_all( path_, error ) );
                error.clear();
                const bool created = std::filesystem::create_directories( path_, error );
                EXPECT_TRUE( created );
                EXPECT_FALSE( error );
            }

            ~TempRotation() noexcept
            {
                std::error_code error;
                static_cast<void>( std::filesystem::remove_all( path_, error ) );
            }

            TempRotation( const TempRotation& ) = delete;
            TempRotation&
            operator=( const TempRotation& ) = delete;
            TempRotation( TempRotation&& )   = delete;
            TempRotation&
            operator=( TempRotation&& ) = delete;

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
                return std::string{ tempPrefix } +
                       info->test_suite_name() +
                       std::string{ nameSeparator } +
                       info->name();
            }

            std::filesystem::path path_;
    };

    [[nodiscard]]
    grab::config::WatchLimits
    limits( std::uint64_t max_files,
            std::uint32_t max_age_days,
            std::uint64_t max_disk_mib )
    {
        return grab::config::WatchLimits{
            .max_files    = max_files,
            .max_age_days = max_age_days,
            .max_disk_mib = max_disk_mib,
        };
    }

    void
    write_file( const std::filesystem::path& path,
                std::uintmax_t               bytes = oneByte )
    {
        std::ofstream stream{ path, std::ios::binary | std::ios::trunc };
        ASSERT_TRUE( stream.is_open() );
        if( bytes != zeroBytes )
        {
            const auto final_offset = static_cast<std::streamoff>( bytes - oneByte );
            stream.seekp( final_offset );
            stream.put( fillerByte );
        }
        ASSERT_TRUE( stream.good() );
    }

    void
    set_modified_at( const std::filesystem::path&          path,
                     std::chrono::system_clock::time_point modified_at )
    {
        const auto file_now   = std::filesystem::file_time_type::clock::now();
        const auto system_now = std::chrono::system_clock::now();
        const auto offset =
            std::chrono::duration_cast<std::filesystem::file_time_type::duration>(
                modified_at - system_now
            );

        std::error_code error;
        std::filesystem::last_write_time( path, file_now + offset, error );
        ASSERT_FALSE( error ) << error.message();
    }

}    // namespace

TEST( ConfigRotation,
      ScanAdoptsOnlyPatternMatches )
{
    const TempRotation temp;
    const auto         now          = std::chrono::system_clock::now();
    const auto         owned_file   = temp.path() / std::string{ firstCaptureName };
    const auto         foreign_file = temp.path() / std::string{ foreignName };
    const auto         current_file = temp.path() / std::string{ secondCaptureName };
    write_file( owned_file );
    write_file( foreign_file );
    set_modified_at( owned_file, now - expiredAge );
    set_modified_at( foreign_file, now - veryOldAge );

    grab::config::RotationLedger ledger(
        temp.path(),
        std::string{ pattern },
        limits( oneFile, unlimitedAgeDays, unlimitedFiles )
    );
    const auto scan_result = ledger.scan();
    ASSERT_TRUE( scan_result.has_value() ) << scan_result.error().message;

    write_file( current_file );
    set_modified_at( current_file, now - recentAge );
    const auto adopt_result = ledger.adopt( current_file );
    ASSERT_TRUE( adopt_result.has_value() ) << adopt_result.error().message;

    EXPECT_FALSE( std::filesystem::exists( owned_file ) );
    EXPECT_TRUE( std::filesystem::exists( current_file ) );
    EXPECT_TRUE( std::filesystem::exists( foreign_file ) );
}

TEST( ConfigRotation,
      MaxFilesDeletesOldest )
{
    const TempRotation temp;
    const auto         now         = std::chrono::system_clock::now();
    const auto         oldest_file = temp.path() / std::string{ firstCaptureName };
    const auto         newest_file = temp.path() / std::string{ secondCaptureName };
    const auto         middle_file = temp.path() / std::string{ thirdCaptureName };
    write_file( oldest_file );
    write_file( newest_file );
    write_file( middle_file );
    set_modified_at( oldest_file, now - oldestAge );
    set_modified_at( middle_file, now - expiredAge );
    set_modified_at( newest_file, now - recentAge );

    grab::config::RotationLedger ledger(
        temp.path(),
        std::string{ pattern },
        limits( twoFiles, unlimitedAgeDays, unlimitedFiles )
    );
    const auto oldest_result = ledger.adopt( oldest_file );
    ASSERT_TRUE( oldest_result.has_value() ) << oldest_result.error().message;
    const auto newest_result = ledger.adopt( newest_file );
    ASSERT_TRUE( newest_result.has_value() ) << newest_result.error().message;
    const auto middle_result = ledger.adopt( middle_file );
    ASSERT_TRUE( middle_result.has_value() ) << middle_result.error().message;

    EXPECT_FALSE( std::filesystem::exists( oldest_file ) );
    EXPECT_TRUE( std::filesystem::exists( middle_file ) );
    EXPECT_TRUE( std::filesystem::exists( newest_file ) );
}

TEST( ConfigRotation,
      PruneAgeRemovesOldLedgerFilesOnly )
{
    const TempRotation temp;
    const auto         now          = std::chrono::system_clock::now();
    const auto         expired_file = temp.path() / std::string{ firstCaptureName };
    const auto         recent_file  = temp.path() / std::string{ secondCaptureName };
    const auto         foreign_file = temp.path() / std::string{ foreignName };
    write_file( expired_file );
    write_file( recent_file );
    write_file( foreign_file );
    set_modified_at( expired_file, now - expiredAge );
    set_modified_at( recent_file, now - recentAge );
    set_modified_at( foreign_file, now - veryOldAge );

    grab::config::RotationLedger ledger(
        temp.path(),
        std::string{ pattern },
        limits( unlimitedFiles, twoAgeDays, unlimitedFiles )
    );
    const auto scan_result = ledger.scan();
    ASSERT_TRUE( scan_result.has_value() ) << scan_result.error().message;
    const auto prune_result = ledger.prune_age( now );
    ASSERT_TRUE( prune_result.has_value() ) << prune_result.error().message;

    EXPECT_EQ( *prune_result, onePrunedFile );
    EXPECT_FALSE( std::filesystem::exists( expired_file ) );
    EXPECT_TRUE( std::filesystem::exists( recent_file ) );
    EXPECT_TRUE( std::filesystem::exists( foreign_file ) );
}

TEST( ConfigRotation,
      DiskCapPauses )
{
    const TempRotation temp;
    const auto         capture_file = temp.path() / std::string{ firstCaptureName };
    grab::config::RotationLedger ledger(
        temp.path(),
        std::string{ pattern },
        limits( unlimitedFiles, unlimitedAgeDays, oneMebibyte )
    );
    const auto scan_result = ledger.scan();
    ASSERT_TRUE( scan_result.has_value() ) << scan_result.error().message;

    write_file( capture_file, overDiskCapBytes );
    const auto adopt_result = ledger.adopt( capture_file );
    ASSERT_TRUE( adopt_result.has_value() ) << adopt_result.error().message;

    EXPECT_TRUE( ledger.paused() );
}

TEST( ConfigRotation,
      ResumesBelowNinetyPercent )
{
    const TempRotation temp;
    const auto         capture_file = temp.path() / std::string{ firstCaptureName };
    grab::config::RotationLedger ledger(
        temp.path(),
        std::string{ pattern },
        limits( unlimitedFiles, unlimitedAgeDays, oneMebibyte )
    );
    const auto scan_result = ledger.scan();
    ASSERT_TRUE( scan_result.has_value() ) << scan_result.error().message;
    write_file( capture_file, overDiskCapBytes );
    const auto adopt_result = ledger.adopt( capture_file );
    ASSERT_TRUE( adopt_result.has_value() ) << adopt_result.error().message;
    ASSERT_TRUE( ledger.paused() );

    write_file( capture_file, atResumeThreshold );
    const auto at_threshold = ledger.refresh_disk();
    ASSERT_TRUE( at_threshold.has_value() ) << at_threshold.error().message;
    EXPECT_TRUE( ledger.paused() );

    write_file( capture_file, belowResumeThreshold );
    const auto below_threshold = ledger.refresh_disk();
    ASSERT_TRUE( below_threshold.has_value() ) << below_threshold.error().message;

    EXPECT_FALSE( ledger.paused() );
}

TEST( ConfigRotation,
      SymlinksNeverDeleted )
{
    const TempRotation temp;
    const auto         now            = std::chrono::system_clock::now();
    const auto         ledger_dir     = temp.path() / std::string{ ledgerDirectoryName };
    const auto         expired_file   = ledger_dir / std::string{ firstCaptureName };
    const auto         symlink_file   = ledger_dir / std::string{ symlinkName };
    const auto         symlink_target = temp.path() / std::string{ symlinkTargetName };
    const auto dangling_symlink       = ledger_dir / std::string{ danglingSymlinkName };
    const auto missing_target         = temp.path() / std::string{ missingTargetName };
    const auto directory_symlink      = ledger_dir / std::string{ directorySymlinkName };
    const auto missing_directory = temp.path() / std::string{ missingDirectoryName };

    std::error_code error;
    const bool      created = std::filesystem::create_directory( ledger_dir, error );
    ASSERT_TRUE( created );
    ASSERT_FALSE( error );
    write_file( expired_file );
    write_file( symlink_target );
    set_modified_at( expired_file, now - expiredAge );
    set_modified_at( symlink_target, now - veryOldAge );
    std::filesystem::create_symlink( symlink_target, symlink_file, error );
    ASSERT_FALSE( error ) << error.message();
    std::filesystem::create_symlink( missing_target, dangling_symlink, error );
    ASSERT_FALSE( error ) << error.message();
    std::filesystem::create_directory_symlink( missing_directory,
                                               directory_symlink,
                                               error );
    ASSERT_FALSE( error ) << error.message();

    grab::config::RotationLedger ledger(
        ledger_dir,
        std::string{ pattern },
        limits( unlimitedFiles, twoAgeDays, unlimitedFiles )
    );
    const auto scan_result = ledger.scan();
    ASSERT_TRUE( scan_result.has_value() ) << scan_result.error().message;
    const auto prune_result = ledger.prune_age( now );
    ASSERT_TRUE( prune_result.has_value() ) << prune_result.error().message;

    grab::config::RotationLedger linked_ledger(
        ledger_dir,
        std::string{ linkedDirectoryPattern },
        limits( unlimitedFiles, twoAgeDays, unlimitedFiles )
    );
    const auto linked_scan = linked_ledger.scan();
    ASSERT_TRUE( linked_scan.has_value() ) << linked_scan.error().message;

    EXPECT_EQ( *prune_result, onePrunedFile );
    EXPECT_FALSE( std::filesystem::exists( expired_file ) );
    EXPECT_TRUE(
        std::filesystem::is_symlink( std::filesystem::symlink_status( symlink_file ) )
    );
    EXPECT_TRUE( std::filesystem::is_symlink(
        std::filesystem::symlink_status( dangling_symlink )
    ) );
    EXPECT_TRUE( std::filesystem::is_symlink(
        std::filesystem::symlink_status( directory_symlink )
    ) );
    EXPECT_TRUE( std::filesystem::exists( symlink_target ) );
}

TEST( ConfigRotation,
      NestedPatternOwnsNestedFiles )
{
    const TempRotation temp;
    const auto         now         = std::chrono::system_clock::now();
    const auto         nested_dir  = temp.path() / std::string{ nestedDirectoryName };
    const auto         oldest_file = nested_dir / std::string{ firstCaptureName };
    const auto         newest_file = nested_dir / std::string{ secondCaptureName };

    std::error_code    error;
    const bool         created = std::filesystem::create_directory( nested_dir, error );
    ASSERT_TRUE( created );
    ASSERT_FALSE( error );
    write_file( oldest_file );
    set_modified_at( oldest_file, now - oldestAge );

    grab::config::RotationLedger ledger(
        temp.path(),
        std::string{ nestedPattern },
        limits( oneFile, unlimitedAgeDays, unlimitedFiles )
    );
    const auto scan_result = ledger.scan();
    ASSERT_TRUE( scan_result.has_value() ) << scan_result.error().message;

    write_file( newest_file );
    set_modified_at( newest_file, now - recentAge );
    const auto adopt_result = ledger.adopt( newest_file );
    ASSERT_TRUE( adopt_result.has_value() ) << adopt_result.error().message;

    EXPECT_FALSE( std::filesystem::exists( oldest_file ) );
    EXPECT_TRUE( std::filesystem::exists( newest_file ) );
}

TEST( ConfigRotation,
      MaximumAgeDoesNotOverflow )
{
    const TempRotation temp;
    const auto         now          = std::chrono::system_clock::now();
    const auto         capture_file = temp.path() / std::string{ firstCaptureName };
    write_file( capture_file );
    set_modified_at( capture_file, now - veryOldAge );

    grab::config::RotationLedger ledger(
        temp.path(),
        std::string{ pattern },
        limits( unlimitedFiles, maximumAgeDays, unlimitedFiles )
    );
    const auto scan_result = ledger.scan();
    ASSERT_TRUE( scan_result.has_value() ) << scan_result.error().message;
    const auto prune_result = ledger.prune_age( now );
    ASSERT_TRUE( prune_result.has_value() ) << prune_result.error().message;

    EXPECT_EQ( *prune_result, noPrunedFiles );
    EXPECT_TRUE( std::filesystem::exists( capture_file ) );
}
