#include "grab/event.hpp"
#include "grab/result.hpp"
#include "storage/jsonl_sink.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
// clang-format on

namespace
{

    constexpr std::string_view kTempRootName       = "jsonl_sink_tmp";
    constexpr std::string_view kFirstDayFileName   = "2024-01-01.jsonl";
    constexpr std::string_view kSecondDayFileName  = "2024-01-02.jsonl";
    constexpr std::string_view kThirdDayFileName   = "2024-01-03.jsonl";
    constexpr std::string_view kFourthDayFileName  = "2024-01-04.jsonl";
    constexpr std::string_view kFifthDayFileName   = "2024-01-05.jsonl";
    constexpr std::string_view kTypeKey            = "type";
    constexpr std::string_view kTierKey            = "tier";
    constexpr std::string_view kTimestampKey       = "ts";
    constexpr std::string_view kDataKey            = "data";
    constexpr std::string_view kKeyDownTypeName    = "input.key_down";
    constexpr std::string_view kInputTierName      = "input";
    constexpr std::string_view kKeyName            = "A";
    constexpr char             kJsonQuote          = '"';
    constexpr char             kJsonEscape         = '\\';
    constexpr char             kFillerByte         = 'x';
    constexpr double           kFirstDayTimestamp  = 1'704'067'200.0;
    constexpr double           kSecondsPerDay      = 86'400.0;
    constexpr std::uint64_t    kFirstSequence      = 1U;
    constexpr std::uint64_t    kSecondSequence     = kFirstSequence + 1U;
    constexpr std::uint32_t    kKeyCode            = 30U;
    constexpr double           kOneSecond          = 1.0;
    constexpr double           kThirdDayOffsetDays = 2.0;
    constexpr std::size_t      kBufferLimit        = 3U;
    constexpr std::size_t      kFlushEveryWrite    = 1U;
    constexpr std::size_t      kSingleLineCount    = 1U;
    constexpr std::size_t      kBufferedOnlyWrites = kBufferLimit - 1U;
    constexpr std::size_t      kMaxFiles           = 3U;
    constexpr std::size_t      kRetentionWrites    = 5U;
    constexpr std::size_t      kGenerousFileLimit  = 10U;
    constexpr std::size_t      kTinyDiskBudgetMb   = 1U;
    constexpr std::uintmax_t   kBytesPerKilobyte   = 1'024U;
    constexpr std::uintmax_t   kBytesPerMegabyte = kBytesPerKilobyte * kBytesPerKilobyte;
    constexpr std::uintmax_t   kOldFileKilobytes = 700U;
    constexpr std::uintmax_t   kOldFileBytes     = kOldFileKilobytes * kBytesPerKilobyte;
    constexpr std::uintmax_t   kTinyDiskBudgetBytes =
        kTinyDiskBudgetMb * kBytesPerMegabyte;

    class TempDir
    {
        public:

            explicit TempDir( std::string_view name ) :
                path_( std::filesystem::current_path() /
                       std::string{ kTempRootName } /
                       std::string{ name } )
            {
                std::error_code ec;
                std::filesystem::remove_all( path_, ec );
                std::filesystem::create_directories( path_, ec );
            }

            ~TempDir() noexcept
            {
                std::error_code ec;
                std::filesystem::remove_all( path_, ec );
            }

            TempDir( const TempDir& ) = delete;
            TempDir&
            operator=( const TempDir& ) = delete;
            TempDir( TempDir&& )        = delete;
            TempDir&
            operator=( TempDir&& ) = delete;

            [[nodiscard]]
            const std::filesystem::path&
            path() const noexcept
            {
                return path_;
            }

        private:

            std::filesystem::path path_;
    };

    [[nodiscard]]
    grab::Event
    make_key_event( double        timestamp,
                    std::uint64_t sequence )
    {
        return grab::Event{
            .timestamp = timestamp,
            .sequence  = sequence,
            .kind      = grab::EventKind::key_down,
            .category  = grab::EventCategory::input,
            .payload   = grab::Payload{ grab::InputKey{
                .code = kKeyCode,
                .name = std::string{ kKeyName },
            } },
        };
    }

    template<typename T>
    [[nodiscard]]
    testing::AssertionResult
    is_ok( const grab::Result<T>& result )
    {
        if( result.has_value() )
        {
            return testing::AssertionSuccess();
        }
        return testing::AssertionFailure() << result.error().message;
    }

    [[nodiscard]]
    grab::storage::JsonlOptions
    make_options( const std::filesystem::path& dir,
                  std::size_t                  buffer_limit,
                  std::size_t                  max_files,
                  std::size_t                  max_disk_mb )
    {
        return grab::storage::JsonlOptions{
            .dir          = dir,
            .buffer_limit = buffer_limit,
            .max_files    = max_files,
            .max_disk_mb  = max_disk_mb,
        };
    }

    [[nodiscard]]
    std::vector<std::string>
    read_lines( const std::filesystem::path& file )
    {
        std::ifstream            input( file );
        std::vector<std::string> lines;
        std::string              line;
        while( std::getline( input, line ) )
        {
            lines.push_back( line );
        }
        return lines;
    }

    [[nodiscard]]
    std::vector<std::string>
    jsonl_file_names( const std::filesystem::path& dir )
    {
        std::vector<std::string> names;
        for( const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator( dir ) )
        {
            if( entry.path().extension() == ".jsonl" )
            {
                names.push_back( entry.path().filename().string() );
            }
        }
        std::ranges::sort( names );
        return names;
    }

    [[nodiscard]]
    std::optional<std::string>
    json_string_field( std::string_view line,
                       std::string_view key )
    {
        const std::string needle = "\"" + std::string{ key } + "\":\"";
        std::size_t       pos    = line.find( needle );
        if( pos == std::string_view::npos )
        {
            return std::nullopt;
        }
        pos += needle.size();

        std::string value;
        bool        escaped = false;
        for( ; pos < line.size(); ++pos )
        {
            const char current = line.at( pos );
            if( escaped )
            {
                value.push_back( current );
                escaped = false;
                continue;
            }
            if( current == kJsonEscape )
            {
                escaped = true;
                continue;
            }
            if( current == kJsonQuote )
            {
                return value;
            }
            value.push_back( current );
        }
        return std::nullopt;
    }

    [[nodiscard]]
    bool
    has_json_key( std::string_view line,
                  std::string_view key )
    {
        const std::string needle = "\"" + std::string{ key } + "\":";
        return line.contains( needle );
    }

    void
    write_filler_file( const std::filesystem::path& file,
                       std::uintmax_t               bytes )
    {
        std::ofstream     output( file, std::ios::binary );
        const std::string chunk( static_cast<std::size_t>( kBytesPerKilobyte ),
                                 kFillerByte );
        std::uintmax_t    remaining = bytes;
        while( remaining != 0U )
        {
            const std::uintmax_t count =
                std::min( remaining, static_cast<std::uintmax_t>( chunk.size() ) );
            output.write( chunk.data(), static_cast<std::streamsize>( count ) );
            remaining -= count;
        }
    }

    [[nodiscard]]
    std::uintmax_t
    total_jsonl_bytes( const std::filesystem::path& dir )
    {
        std::uintmax_t total = 0U;
        for( const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator( dir ) )
        {
            if( entry.path().extension() == ".jsonl" )
            {
                total += entry.file_size();
            }
        }
        return total;
    }

}    // namespace

TEST( JsonlSink,
      BuffersUntilLimitThenFlushes )
{
    const TempDir temp( "BuffersUntilLimitThenFlushes" );
    auto          sink_result = grab::storage::JsonlSink::open(
        make_options( temp.path(), kBufferLimit, kGenerousFileLimit, kTinyDiskBudgetMb )
    );
    ASSERT_TRUE( is_ok( sink_result ) );
    auto sink = std::move( sink_result ).value();

    for( std::size_t index = 0U; index < kBufferedOnlyWrites; ++index )
    {
        const auto write_result = sink.write(
            make_key_event( kFirstDayTimestamp + static_cast<double>( index ),
                            kFirstSequence + index )
        );
        ASSERT_TRUE( is_ok( write_result ) );
    }

    const auto file = temp.path() / std::string{ kFirstDayFileName };
    EXPECT_FALSE( std::filesystem::exists( file ) );

    const auto write_result = sink.write(
        make_key_event( kFirstDayTimestamp + static_cast<double>( kBufferedOnlyWrites ),
                        kFirstSequence + kBufferedOnlyWrites )
    );
    ASSERT_TRUE( is_ok( write_result ) );

    EXPECT_TRUE( std::filesystem::exists( file ) );
    EXPECT_EQ( read_lines( file ).size(), kBufferLimit );
}

TEST( JsonlSink,
      WritesDailyFileByEventDate )
{
    const TempDir temp( "WritesDailyFileByEventDate" );
    auto          sink_result = grab::storage::JsonlSink::open(
        make_options( temp.path(), kBufferLimit, kGenerousFileLimit, kTinyDiskBudgetMb )
    );
    ASSERT_TRUE( is_ok( sink_result ) );
    auto sink = std::move( sink_result ).value();

    ASSERT_TRUE( is_ok( sink.write( make_key_event( kFirstDayTimestamp,
                                                    kFirstSequence ) ) ) );
    ASSERT_TRUE( is_ok( sink.write( make_key_event( kFirstDayTimestamp + kSecondsPerDay,
                                                    kSecondSequence ) ) ) );
    ASSERT_TRUE( is_ok( sink.flush() ) );

    EXPECT_TRUE( std::filesystem::exists( temp.path() /
                                          std::string{ kFirstDayFileName } ) );
    EXPECT_TRUE( std::filesystem::exists( temp.path() /
                                          std::string{ kSecondDayFileName } ) );
}

TEST( JsonlSink,
      LineFormatHasTsTypeTierData )
{
    const TempDir temp( "LineFormatHasTsTypeTierData" );
    auto          sink_result = grab::storage::JsonlSink::open(
        make_options( temp.path(), kBufferLimit, kGenerousFileLimit, kTinyDiskBudgetMb )
    );
    ASSERT_TRUE( is_ok( sink_result ) );
    auto sink = std::move( sink_result ).value();

    ASSERT_TRUE( is_ok( sink.write( make_key_event( kFirstDayTimestamp,
                                                    kFirstSequence ) ) ) );
    ASSERT_TRUE( is_ok( sink.flush() ) );

    const auto lines = read_lines( temp.path() / std::string{ kFirstDayFileName } );
    ASSERT_EQ( lines.size(), kSingleLineCount );
    const std::string_view line = lines.front();

    EXPECT_TRUE( has_json_key( line, kTimestampKey ) );
    EXPECT_TRUE( has_json_key( line, kTypeKey ) );
    EXPECT_TRUE( has_json_key( line, kTierKey ) );
    EXPECT_TRUE( has_json_key( line, kDataKey ) );
    EXPECT_EQ( json_string_field( line, kTypeKey ).value_or( std::string{} ),
               kKeyDownTypeName );
    EXPECT_EQ( json_string_field( line, kTierKey ).value_or( std::string{} ),
               kInputTierName );
}

TEST( JsonlSink,
      MaxFilesPrunesOldest )
{
    const TempDir temp( "MaxFilesPrunesOldest" );
    auto          sink_result = grab::storage::JsonlSink::open(
        make_options( temp.path(), kFlushEveryWrite, kMaxFiles, kTinyDiskBudgetMb )
    );
    ASSERT_TRUE( is_ok( sink_result ) );
    auto sink = std::move( sink_result ).value();

    for( std::size_t index = 0U; index < kRetentionWrites; ++index )
    {
        ASSERT_TRUE( is_ok( sink.write( make_key_event(
            kFirstDayTimestamp + ( static_cast<double>( index ) * kSecondsPerDay ),
            kFirstSequence + index
        ) ) ) );
    }

    const std::vector<std::string> expected_names{
        std::string{ kThirdDayFileName },
        std::string{ kFourthDayFileName },
        std::string{ kFifthDayFileName },
    };
    EXPECT_EQ( jsonl_file_names( temp.path() ), expected_names );
}

TEST( JsonlSink,
      MaxDiskMbPrunesBySize )
{
    const TempDir temp( "MaxDiskMbPrunesBySize" );
    write_filler_file( temp.path() / std::string{ kFirstDayFileName }, kOldFileBytes );
    write_filler_file( temp.path() / std::string{ kSecondDayFileName }, kOldFileBytes );

    auto sink_result =
        grab::storage::JsonlSink::open( make_options( temp.path(),
                                                      kFlushEveryWrite,
                                                      kGenerousFileLimit,
                                                      kTinyDiskBudgetMb ) );
    ASSERT_TRUE( is_ok( sink_result ) );
    auto sink = std::move( sink_result ).value();
    ASSERT_TRUE( is_ok( sink.write(
        make_key_event( kFirstDayTimestamp + ( kSecondsPerDay * kThirdDayOffsetDays ),
                        kFirstSequence )
    ) ) );

    EXPECT_FALSE( std::filesystem::exists( temp.path() /
                                           std::string{ kFirstDayFileName } ) );
    EXPECT_TRUE( std::filesystem::exists( temp.path() /
                                          std::string{ kSecondDayFileName } ) );
    EXPECT_TRUE( std::filesystem::exists( temp.path() /
                                          std::string{ kThirdDayFileName } ) );
    EXPECT_LE( total_jsonl_bytes( temp.path() ), kTinyDiskBudgetBytes );
}

TEST( JsonlSink,
      DestructorFlushes )
{
    const TempDir temp( "DestructorFlushes" );
    {
        auto sink_result =
            grab::storage::JsonlSink::open( make_options( temp.path(),
                                                          kBufferLimit,
                                                          kGenerousFileLimit,
                                                          kTinyDiskBudgetMb ) );
        ASSERT_TRUE( is_ok( sink_result ) );
        auto sink = std::move( sink_result ).value();
        ASSERT_TRUE( is_ok( sink.write( make_key_event( kFirstDayTimestamp,
                                                        kFirstSequence ) ) ) );
        ASSERT_TRUE( is_ok( sink.write( make_key_event( kFirstDayTimestamp + kOneSecond,
                                                        kSecondSequence ) ) ) );
    }

    EXPECT_EQ( read_lines( temp.path() / std::string{ kFirstDayFileName } ).size(),
               kBufferedOnlyWrites );
}
