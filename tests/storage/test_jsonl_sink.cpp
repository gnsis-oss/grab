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

    constexpr std::string_view tempRootName        = "jsonl_sink_tmp";
    constexpr std::string_view firstDayFileName    = "2024-01-01.jsonl";
    constexpr std::string_view secondDayFileName   = "2024-01-02.jsonl";
    constexpr std::string_view thirdDayFileName    = "2024-01-03.jsonl";
    constexpr std::string_view fourthDayFileName   = "2024-01-04.jsonl";
    constexpr std::string_view fifthDayFileName    = "2024-01-05.jsonl";
    constexpr std::string_view preEpochFileName    = "1969-12-31.jsonl";
    constexpr std::string_view leapDayFileName     = "2024-02-29.jsonl";
    constexpr std::string_view typeKey             = "type";
    constexpr std::string_view categoryKey         = "category";
    constexpr std::string_view oldTierKey          = "tier";
    constexpr std::string_view timestampKey        = "ts";
    constexpr std::string_view dataKey             = "data";
    constexpr std::string_view keyDownTypeName     = "input.key_down";
    constexpr std::string_view inputCategoryName   = "input";
    constexpr std::string_view keyCodeKey          = "key_code";
    constexpr std::string_view keyNameKey          = "key_name";
    constexpr std::string_view oldCodeKey          = "code";
    constexpr std::string_view oldNameKey          = "name";
    constexpr std::string_view buttonKey           = "button";
    constexpr std::string_view buttonNameKey       = "button_name";
    constexpr std::string_view stateKey            = "state";
    constexpr std::string_view detailKey           = "detail";
    constexpr std::string_view keyName             = "A";
    constexpr std::string_view mouseButtonName     = "left";
    constexpr std::string_view a11yApp             = "Editor";
    constexpr std::string_view a11yRole            = "toggle";
    constexpr std::string_view a11yName            = "Sidebar";
    constexpr std::string_view a11yState           = "checked";
    constexpr char             jsonQuote           = '"';
    constexpr char             jsonEscape          = '\\';
    constexpr char             fillerByte          = 'x';
    constexpr double           firstDayTimestamp   = 1'704'067'200.0;
    constexpr double           preEpochTimestamp   = -0.25;
    constexpr double           leapDayTimestamp    = 1'709'164'800.0;
    constexpr double           secondsPerDay       = 86'400.0;
    constexpr std::uint64_t    firstSequence       = 1U;
    constexpr std::uint64_t    secondSequence      = firstSequence + 1U;
    constexpr std::uint32_t    keyCode             = 30U;
    constexpr std::uint32_t    mouseButton         = 1U;
    constexpr double           oneSecond           = 1.0;
    constexpr double           thirdDayOffsetDays  = 2.0;
    constexpr std::size_t      bufferLimit         = 3U;
    constexpr std::size_t      flushEveryWrite     = 1U;
    constexpr std::size_t      singleLineCount     = 1U;
    constexpr std::size_t      bufferedOnlyWrites  = bufferLimit - 1U;
    constexpr std::size_t      maxFiles            = 3U;
    constexpr std::size_t      retentionWrites     = 5U;
    constexpr std::size_t      generousFileLimit   = 10U;
    constexpr std::size_t      tinyDiskBudgetMb    = 1U;
    constexpr std::uintmax_t   bytesPerKilobyte    = 1'024U;
    constexpr std::uintmax_t   bytesPerMegabyte    = bytesPerKilobyte * bytesPerKilobyte;
    constexpr std::uintmax_t   oldFileKilobytes    = 700U;
    constexpr std::uintmax_t   oldFileBytes        = oldFileKilobytes * bytesPerKilobyte;
    constexpr std::uintmax_t   tinyDiskBudgetBytes = tinyDiskBudgetMb * bytesPerMegabyte;

    class TempDir
    {
        public:

            explicit TempDir( std::string_view name ) :
                path_( std::filesystem::current_path() /
                       std::string{ tempRootName } /
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
            .kind      = grab::EventKind::KeyDown,
            .category  = grab::EventCategory::Input,
            .payload   = grab::Payload{ grab::InputKey{
                .code = keyCode,
                .name = std::string{ keyName },
            } },
        };
    }

    [[nodiscard]]
    grab::Event
    make_mouse_click_event( double        timestamp,
                            std::uint64_t sequence )
    {
        return grab::Event{
            .timestamp = timestamp,
            .sequence  = sequence,
            .kind      = grab::EventKind::MouseClick,
            .category  = grab::EventCategory::Input,
            .payload   = grab::Payload{ grab::MouseClick{
                .button = mouseButton,
                .name   = std::string{ mouseButtonName },
            } },
        };
    }

    [[nodiscard]]
    grab::Event
    make_a11y_state_event( double        timestamp,
                           std::uint64_t sequence )
    {
        return grab::Event{
            .timestamp = timestamp,
            .sequence  = sequence,
            .kind      = grab::EventKind::A11yStateChanged,
            .category  = grab::EventCategory::Accessibility,
            .payload   = grab::Payload{ grab::A11yEvent{
                .app    = std::string{ a11yApp },
                .role   = std::string{ a11yRole },
                .name   = std::string{ a11yName },
                .detail = std::string{ a11yState },
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
            if( current == jsonEscape )
            {
                escaped = true;
                continue;
            }
            if( current == jsonQuote )
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
        const std::string chunk( static_cast<std::size_t>( bytesPerKilobyte ),
                                 fillerByte );
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
        make_options( temp.path(), bufferLimit, generousFileLimit, tinyDiskBudgetMb )
    );
    ASSERT_TRUE( is_ok( sink_result ) );
    auto sink = std::move( sink_result ).value();

    for( std::size_t index = 0U; index < bufferedOnlyWrites; ++index )
    {
        const auto write_result =
            sink.write( make_key_event( firstDayTimestamp + static_cast<double>( index ),
                                        firstSequence + index ) );
        ASSERT_TRUE( is_ok( write_result ) );
    }

    const auto file = temp.path() / std::string{ firstDayFileName };
    EXPECT_FALSE( std::filesystem::exists( file ) );

    const auto write_result = sink.write(
        make_key_event( firstDayTimestamp + static_cast<double>( bufferedOnlyWrites ),
                        firstSequence + bufferedOnlyWrites )
    );
    ASSERT_TRUE( is_ok( write_result ) );

    EXPECT_TRUE( std::filesystem::exists( file ) );
    EXPECT_EQ( read_lines( file ).size(), bufferLimit );
}

TEST( JsonlSink,
      WritesDailyFileByEventDate )
{
    const TempDir temp( "WritesDailyFileByEventDate" );
    auto          sink_result = grab::storage::JsonlSink::open(
        make_options( temp.path(), bufferLimit, generousFileLimit, tinyDiskBudgetMb )
    );
    ASSERT_TRUE( is_ok( sink_result ) );
    auto sink = std::move( sink_result ).value();

    ASSERT_TRUE( is_ok( sink.write( make_key_event( firstDayTimestamp,
                                                    firstSequence ) ) ) );
    ASSERT_TRUE( is_ok( sink.write( make_key_event( firstDayTimestamp + secondsPerDay,
                                                    secondSequence ) ) ) );
    ASSERT_TRUE( is_ok( sink.flush() ) );

    EXPECT_TRUE( std::filesystem::exists( temp.path() /
                                          std::string{ firstDayFileName } ) );
    EXPECT_TRUE( std::filesystem::exists( temp.path() /
                                          std::string{ secondDayFileName } ) );
}

TEST( JsonlSink,
      UsesCalendarDatesAcrossEpochAndLeapDay )
{
    const TempDir temp( "UsesCalendarDatesAcrossEpochAndLeapDay" );
    auto          sink_result = grab::storage::JsonlSink::open(
        make_options( temp.path(), bufferLimit, generousFileLimit, tinyDiskBudgetMb )
    );
    ASSERT_TRUE( is_ok( sink_result ) );
    auto sink = std::move( sink_result ).value();

    ASSERT_TRUE( is_ok( sink.write( make_key_event( preEpochTimestamp,
                                                    firstSequence ) ) ) );
    ASSERT_TRUE( is_ok( sink.write( make_key_event( leapDayTimestamp,
                                                    secondSequence ) ) ) );
    ASSERT_TRUE( is_ok( sink.flush() ) );

    EXPECT_TRUE( std::filesystem::exists( temp.path() /
                                          std::string{ preEpochFileName } ) );
    EXPECT_TRUE( std::filesystem::exists( temp.path() /
                                          std::string{ leapDayFileName } ) );
}

TEST( JsonlSink,
      LineFormatHasTsTypeCategoryData )
{
    const TempDir temp( "LineFormatHasTsTypeCategoryData" );
    auto          sink_result = grab::storage::JsonlSink::open(
        make_options( temp.path(), bufferLimit, generousFileLimit, tinyDiskBudgetMb )
    );
    ASSERT_TRUE( is_ok( sink_result ) );
    auto sink = std::move( sink_result ).value();

    ASSERT_TRUE( is_ok( sink.write( make_key_event( firstDayTimestamp,
                                                    firstSequence ) ) ) );
    ASSERT_TRUE( is_ok( sink.flush() ) );

    const auto lines = read_lines( temp.path() / std::string{ firstDayFileName } );
    ASSERT_EQ( lines.size(), singleLineCount );
    const std::string_view line = lines.front();

    EXPECT_TRUE( has_json_key( line, timestampKey ) );
    EXPECT_TRUE( has_json_key( line, typeKey ) );
    EXPECT_TRUE( has_json_key( line, categoryKey ) );
    EXPECT_FALSE( has_json_key( line, oldTierKey ) );
    EXPECT_TRUE( has_json_key( line, dataKey ) );
    EXPECT_EQ( json_string_field( line, typeKey ).value_or( std::string{} ),
               keyDownTypeName );
    EXPECT_EQ( json_string_field( line, categoryKey ).value_or( std::string{} ),
               inputCategoryName );
}

TEST( JsonlSink,
      SerializesInputKeyPayloadWithCanonicalKeys )
{
    const TempDir temp( "SerializesInputKeyPayloadWithCanonicalKeys" );
    auto          sink_result = grab::storage::JsonlSink::open(
        make_options( temp.path(), flushEveryWrite, generousFileLimit, tinyDiskBudgetMb )
    );
    ASSERT_TRUE( is_ok( sink_result ) );
    auto sink = std::move( sink_result ).value();

    ASSERT_TRUE( is_ok( sink.write( make_key_event( firstDayTimestamp,
                                                    firstSequence ) ) ) );

    const auto lines = read_lines( temp.path() / std::string{ firstDayFileName } );
    ASSERT_EQ( lines.size(), singleLineCount );
    const std::string_view line = lines.front();

    EXPECT_TRUE( has_json_key( line, keyCodeKey ) );
    EXPECT_TRUE( has_json_key( line, keyNameKey ) );
    EXPECT_FALSE( has_json_key( line, oldCodeKey ) );
    EXPECT_FALSE( has_json_key( line, oldNameKey ) );
}

TEST( JsonlSink,
      SerializesMouseClickPayloadWithCanonicalButtonName )
{
    const TempDir temp( "SerializesMouseClickPayloadWithCanonicalButtonName" );
    auto          sink_result = grab::storage::JsonlSink::open(
        make_options( temp.path(), flushEveryWrite, generousFileLimit, tinyDiskBudgetMb )
    );
    ASSERT_TRUE( is_ok( sink_result ) );
    auto sink = std::move( sink_result ).value();

    ASSERT_TRUE( is_ok( sink.write( make_mouse_click_event( firstDayTimestamp,
                                                            firstSequence ) ) ) );

    const auto lines = read_lines( temp.path() / std::string{ firstDayFileName } );
    ASSERT_EQ( lines.size(), singleLineCount );
    const std::string_view line = lines.front();

    EXPECT_TRUE( has_json_key( line, buttonKey ) );
    EXPECT_TRUE( has_json_key( line, buttonNameKey ) );
    EXPECT_FALSE( has_json_key( line, oldNameKey ) );
}

TEST( JsonlSink,
      SerializesA11yStateChangedPayloadWithStateKey )
{
    const TempDir temp( "SerializesA11yStateChangedPayloadWithStateKey" );
    auto          sink_result = grab::storage::JsonlSink::open(
        make_options( temp.path(), flushEveryWrite, generousFileLimit, tinyDiskBudgetMb )
    );
    ASSERT_TRUE( is_ok( sink_result ) );
    auto sink = std::move( sink_result ).value();

    ASSERT_TRUE( is_ok( sink.write( make_a11y_state_event( firstDayTimestamp,
                                                           firstSequence ) ) ) );

    const auto lines = read_lines( temp.path() / std::string{ firstDayFileName } );
    ASSERT_EQ( lines.size(), singleLineCount );
    const std::string_view line = lines.front();

    EXPECT_TRUE( has_json_key( line, stateKey ) );
    EXPECT_FALSE( has_json_key( line, detailKey ) );
}

TEST( JsonlSink,
      MaxFilesPrunesOldest )
{
    const TempDir temp( "MaxFilesPrunesOldest" );
    auto          sink_result = grab::storage::JsonlSink::open(
        make_options( temp.path(), flushEveryWrite, maxFiles, tinyDiskBudgetMb )
    );
    ASSERT_TRUE( is_ok( sink_result ) );
    auto sink = std::move( sink_result ).value();

    for( std::size_t index = 0U; index < retentionWrites; ++index )
    {
        ASSERT_TRUE( is_ok( sink.write( make_key_event(
            firstDayTimestamp + ( static_cast<double>( index ) * secondsPerDay ),
            firstSequence + index
        ) ) ) );
    }

    const std::vector<std::string> expected_names{
        std::string{ thirdDayFileName },
        std::string{ fourthDayFileName },
        std::string{ fifthDayFileName },
    };
    EXPECT_EQ( jsonl_file_names( temp.path() ), expected_names );
}

TEST( JsonlSink,
      MaxDiskMbPrunesBySize )
{
    const TempDir temp( "MaxDiskMbPrunesBySize" );
    write_filler_file( temp.path() / std::string{ firstDayFileName }, oldFileBytes );
    write_filler_file( temp.path() / std::string{ secondDayFileName }, oldFileBytes );

    auto sink_result = grab::storage::JsonlSink::open(
        make_options( temp.path(), flushEveryWrite, generousFileLimit, tinyDiskBudgetMb )
    );
    ASSERT_TRUE( is_ok( sink_result ) );
    auto sink = std::move( sink_result ).value();
    ASSERT_TRUE( is_ok( sink.write(
        make_key_event( firstDayTimestamp + ( secondsPerDay * thirdDayOffsetDays ),
                        firstSequence )
    ) ) );

    EXPECT_FALSE( std::filesystem::exists( temp.path() /
                                           std::string{ firstDayFileName } ) );
    EXPECT_TRUE( std::filesystem::exists( temp.path() /
                                          std::string{ secondDayFileName } ) );
    EXPECT_TRUE( std::filesystem::exists( temp.path() /
                                          std::string{ thirdDayFileName } ) );
    EXPECT_LE( total_jsonl_bytes( temp.path() ), tinyDiskBudgetBytes );
}

TEST( JsonlSink,
      DestructorFlushes )
{
    const TempDir temp( "DestructorFlushes" );
    {
        auto sink_result = grab::storage::JsonlSink::open(
            make_options( temp.path(), bufferLimit, generousFileLimit, tinyDiskBudgetMb )
        );
        ASSERT_TRUE( is_ok( sink_result ) );
        auto sink = std::move( sink_result ).value();
        ASSERT_TRUE( is_ok( sink.write( make_key_event( firstDayTimestamp,
                                                        firstSequence ) ) ) );
        ASSERT_TRUE( is_ok( sink.write( make_key_event( firstDayTimestamp + oneSecond,
                                                        secondSequence ) ) ) );
    }

    EXPECT_EQ( read_lines( temp.path() / std::string{ firstDayFileName } ).size(),
               bufferedOnlyWrites );
}
