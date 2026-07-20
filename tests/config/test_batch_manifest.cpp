#include "config/batch_manifest.hpp"
#include "grab/result.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
// clang-format on

namespace
{

    constexpr std::string_view tempPrefix                = "grab-batch-manifest-";
    constexpr std::string_view nameSeparator             = "-";
    constexpr std::string_view manifestFilename          = "manifest.json";
    constexpr std::string_view temporaryManifestFilename = "manifest.json.tmp";

    constexpr std::string_view profilePath               = "/profiles/desktop.json";
    constexpr std::string_view replacementProfilePath    = "/profiles/replacement.json";
    constexpr std::string_view startedAt                 = "2026-07-20T10:15:30.250Z";
    constexpr std::string_view endedAt                   = "2026-07-20T10:16:45.500Z";
    constexpr std::string_view replacementEndedAt        = "2026-07-20T10:17:00.000Z";
    constexpr std::string_view emptyEndedAt              = "";
    constexpr std::string_view targetName                = "text-editor";
    constexpr std::string_view executable                = "/usr/bin/editor";
    constexpr std::string_view argument                  = "--new-window";
    constexpr std::string_view firstCapture              = "text-editor.png";
    constexpr std::string_view secondCapture             = "text-editor_002.png";
    constexpr std::string_view targetError               = "window closed after capture";
    constexpr std::string_view comparedFile              = "text-editor.png";
    constexpr std::int64_t     targetPid                 = 24'680;
    constexpr std::uint32_t    targetWindowId            = 0X01'40'00'03U;
    constexpr double           comparisonScore           = 2.5;
    constexpr bool             comparisonPassed          = true;
    constexpr std::size_t      expectedTargetCount       = 1U;
    constexpr std::size_t      expectedArgumentCount     = 2U;
    constexpr std::size_t      expectedCaptureCount      = 2U;
    constexpr std::size_t      expectedCompareCount      = 1U;

    class TempManifestDirectory
    {
        public:

            TempManifestDirectory() :
                path_( std::filesystem::temp_directory_path() / unique_name() )
            {
                std::error_code error;
                static_cast<void>( std::filesystem::remove_all( path_, error ) );
                error.clear();
                const bool created = std::filesystem::create_directories( path_, error );
                EXPECT_TRUE( created );
                EXPECT_FALSE( error );
            }

            ~TempManifestDirectory() noexcept
            {
                std::error_code error;
                static_cast<void>( std::filesystem::remove_all( path_, error ) );
            }

            TempManifestDirectory( const TempManifestDirectory& ) = delete;
            TempManifestDirectory&
            operator=( const TempManifestDirectory& )        = delete;
            TempManifestDirectory( TempManifestDirectory&& ) = delete;
            TempManifestDirectory&
            operator=( TempManifestDirectory&& ) = delete;

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
    grab::config::BatchManifest
    complete_manifest()
    {
        std::vector<std::string> target_arguments;
        target_arguments.reserve( expectedArgumentCount );
        target_arguments.emplace_back( executable );
        target_arguments.emplace_back( argument );

        std::vector<std::string> captures;
        captures.reserve( expectedCaptureCount );
        captures.emplace_back( firstCapture );
        captures.emplace_back( secondCapture );

        grab::config::TargetOutcome target{
            .name      = std::string{ targetName },
            .argv      = std::move( target_arguments ),
            .pid       = targetPid,
            .window_id = targetWindowId,
            .files     = std::move( captures ),
            .error     = std::string{ targetError },
        };
        grab::config::FileCompareEntry comparison{
            .name   = std::string{ comparedFile },
            .score  = comparisonScore,
            .passed = comparisonPassed,
        };
        grab::config::BatchManifest manifest{
            .profile    = std::filesystem::path{ profilePath },
            .started_at = std::string{ startedAt },
            .ended_at   = std::string{ endedAt },
            .state      = grab::config::RunState::Failed,
            .targets    = {},
            .compare    = {},
        };
        manifest.targets.push_back( std::move( target ) );
        manifest.compare.push_back( std::move( comparison ) );
        return manifest;
    }

    [[nodiscard]]
    std::filesystem::path
    manifest_path( const TempManifestDirectory& directory )
    {
        return directory.path() / std::string{ manifestFilename };
    }

    [[nodiscard]]
    std::filesystem::path
    temporary_manifest_path( const TempManifestDirectory& directory )
    {
        return directory.path() / std::string{ temporaryManifestFilename };
    }

}    // namespace

TEST( BatchManifest,
      WriteThenReadRoundTrips )
{
    const TempManifestDirectory       directory;
    const grab::config::BatchManifest expected     = complete_manifest();

    const auto                        write_result = expected.write( directory.path() );
    ASSERT_TRUE( write_result.has_value() ) << write_result.error().message;

    const auto read_result = grab::config::BatchManifest::read( directory.path() );
    ASSERT_TRUE( read_result.has_value() ) << read_result.error().message;
    EXPECT_EQ( read_result->profile, expected.profile );
    EXPECT_EQ( read_result->started_at, expected.started_at );
    EXPECT_EQ( read_result->ended_at, expected.ended_at );
    EXPECT_EQ( read_result->state, expected.state );

    ASSERT_EQ( read_result->targets.size(), expectedTargetCount );
    ASSERT_EQ( expected.targets.size(), expectedTargetCount );
    const auto& actual_target   = read_result->targets.front();
    const auto& expected_target = expected.targets.front();
    EXPECT_EQ( actual_target.name, expected_target.name );
    ASSERT_EQ( actual_target.argv.size(), expectedArgumentCount );
    EXPECT_EQ( actual_target.argv, expected_target.argv );
    EXPECT_EQ( actual_target.pid, expected_target.pid );
    EXPECT_EQ( actual_target.window_id, expected_target.window_id );
    ASSERT_EQ( actual_target.files.size(), expectedCaptureCount );
    EXPECT_EQ( actual_target.files, expected_target.files );
    EXPECT_EQ( actual_target.error, expected_target.error );

    ASSERT_EQ( read_result->compare.size(), expectedCompareCount );
    ASSERT_EQ( expected.compare.size(), expectedCompareCount );
    const auto& actual_compare   = read_result->compare.front();
    const auto& expected_compare = expected.compare.front();
    EXPECT_EQ( actual_compare.name, expected_compare.name );
    EXPECT_DOUBLE_EQ( actual_compare.score, expected_compare.score );
    EXPECT_EQ( actual_compare.passed, expected_compare.passed );
}

TEST( BatchManifest,
      WriteIsAtomic )
{
    const TempManifestDirectory directory;
    auto                        initial = complete_manifest();
    initial.state                       = grab::config::RunState::Running;
    initial.ended_at                    = std::string{ emptyEndedAt };

    const auto initial_write            = initial.write( directory.path() );
    ASSERT_TRUE( initial_write.has_value() ) << initial_write.error().message;
    EXPECT_TRUE( std::filesystem::exists( manifest_path( directory ) ) );
    EXPECT_FALSE( std::filesystem::exists( temporary_manifest_path( directory ) ) );

    auto replacement     = initial;
    replacement.profile  = std::filesystem::path{ replacementProfilePath };
    replacement.ended_at = std::string{ replacementEndedAt };
    replacement.state    = grab::config::RunState::Done;
    replacement.targets.clear();
    replacement.compare.clear();

    const auto replacement_write = replacement.write( directory.path() );
    ASSERT_TRUE( replacement_write.has_value() ) << replacement_write.error().message;
    EXPECT_FALSE( std::filesystem::exists( temporary_manifest_path( directory ) ) );

    const auto read_result = grab::config::BatchManifest::read( directory.path() );
    ASSERT_TRUE( read_result.has_value() ) << read_result.error().message;
    EXPECT_EQ( read_result->profile, replacement.profile );
    EXPECT_EQ( read_result->started_at, replacement.started_at );
    EXPECT_EQ( read_result->ended_at, replacement.ended_at );
    EXPECT_EQ( read_result->state, replacement.state );
    EXPECT_TRUE( read_result->targets.empty() );
    EXPECT_TRUE( read_result->compare.empty() );
}

TEST( BatchManifest,
      CrashTell )
{
    const TempManifestDirectory       directory;
    const grab::config::BatchManifest running{
        .profile    = std::filesystem::path{ profilePath },
        .started_at = std::string{ startedAt },
        .ended_at   = std::string{ emptyEndedAt },
        .state      = grab::config::RunState::Running,
        .targets    = {},
        .compare    = {},
    };

    const auto write_result = running.write( directory.path() );
    ASSERT_TRUE( write_result.has_value() ) << write_result.error().message;

    const auto read_result = grab::config::BatchManifest::read( directory.path() );
    ASSERT_TRUE( read_result.has_value() ) << read_result.error().message;
    EXPECT_EQ( read_result->state, grab::config::RunState::Running );
}

TEST( BatchManifest,
      ReadMissingManifestIsNotFound )
{
    const TempManifestDirectory directory;

    const auto result = grab::config::BatchManifest::read( directory.path() );

    ASSERT_FALSE( result.has_value() );
    EXPECT_EQ( result.error().code, grab::ErrorCode::InvalidArgument );
}
