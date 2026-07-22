#include "config/pattern.hpp"
#include "grab/result.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
// clang-format on

namespace
{

    constexpr std::chrono::year         fixedYear{ 2'026 };
    constexpr std::chrono::month        fixedMonth = std::chrono::July;
    constexpr std::chrono::day          fixedDay{ 20U };
    constexpr std::chrono::hours        fixedHour{ 10 };
    constexpr std::chrono::minutes      fixedMinute{ 15 };
    constexpr std::chrono::seconds      fixedSecond{ 30 };
    constexpr std::chrono::milliseconds fixedMillisecond{ 250 };
    constexpr std::uint32_t             fixedSequence = 42U;
    constexpr auto                      fixedNow =
        std::chrono::sys_days{ fixedYear / fixedMonth / fixedDay } +
        fixedHour +
        fixedMinute +
        fixedSecond +
        fixedMillisecond;
    constexpr grab::config::PatternContext fixedContext{
        .now = fixedNow,
        .seq = fixedSequence,
    };

    constexpr std::string_view filenamePointer      = "/watch/filename";
    constexpr std::string_view timestampPattern     = "{timestamp}.png";
    constexpr std::string_view timestampExpected    = "20260720T101530.250.png";
    constexpr std::string_view dateTimePattern      = "{date}_{time}";
    constexpr std::string_view dateTimeExpected     = "20260720_101530.png";
    constexpr std::string_view sequencePattern      = "capture_{seq}.png";
    constexpr std::string_view sequenceExpected     = "capture_00042.png";
    constexpr std::string_view extensionlessPattern = "capture";
    constexpr std::string_view appendedPngExpected  = "capture.png";
    constexpr std::string_view explicitPngPattern   = "capture.png";
    constexpr std::string_view unknownTokenPattern  = "capture_{unknown}";
    constexpr std::string_view unknownTokenDetail   = "{unknown}";
    constexpr std::string_view absolutePattern      = "/etc/x";
    constexpr std::string_view relativePathDetail   = "relative";
    constexpr std::string_view dotDotPattern        = "capture/../x";
    constexpr std::string_view dotDotDetail         = "..";
    constexpr std::string_view matchPattern         = "capture_{timestamp}_{seq}";
    constexpr std::string_view foreignName          = "unrelated.txt";
    constexpr std::string_view invalidTimeName = "capture_20260720T251530.250_00042.png";

    void
    expect_invalid_pattern( const grab::Result<std::string>& result,
                            std::string_view                 detail )
    {
        ASSERT_FALSE( result.has_value() );
        EXPECT_EQ( result.error().code, grab::ErrorCode::InvalidArgument );
        EXPECT_NE( result.error().message.find( filenamePointer ), std::string::npos );
        EXPECT_NE( result.error().message.find( detail ), std::string::npos );
    }

}    // namespace

TEST( ConfigPattern,
      TimestampHasMillisecondResolution )
{
    const auto result = grab::config::render_filename( timestampPattern, fixedContext );

    ASSERT_TRUE( result.has_value() ) << result.error().message;
    EXPECT_EQ( *result, timestampExpected );

    const auto date_time =
        grab::config::render_filename( dateTimePattern, fixedContext );
    ASSERT_TRUE( date_time.has_value() ) << date_time.error().message;
    EXPECT_EQ( *date_time, dateTimeExpected );
}

TEST( ConfigPattern,
      SeqZeroPads )
{
    const auto result = grab::config::render_filename( sequencePattern, fixedContext );

    ASSERT_TRUE( result.has_value() ) << result.error().message;
    EXPECT_EQ( *result, sequenceExpected );
}

TEST( ConfigPattern,
      AppendsPngWhenMissing )
{
    const auto result =
        grab::config::render_filename( extensionlessPattern, fixedContext );

    ASSERT_TRUE( result.has_value() ) << result.error().message;
    EXPECT_EQ( *result, appendedPngExpected );
}

TEST( ConfigPattern,
      KeepsExplicitPng )
{
    const auto result =
        grab::config::render_filename( explicitPngPattern, fixedContext );

    ASSERT_TRUE( result.has_value() ) << result.error().message;
    EXPECT_EQ( *result, explicitPngPattern );
}

TEST( ConfigPattern,
      RejectsUnknownToken )
{
    const auto result =
        grab::config::render_filename( unknownTokenPattern, fixedContext );

    expect_invalid_pattern( result, unknownTokenDetail );
}

TEST( ConfigPattern,
      RejectsAbsolute )
{
    const auto result = grab::config::render_filename( absolutePattern, fixedContext );

    expect_invalid_pattern( result, relativePathDetail );
}

TEST( ConfigPattern,
      RejectsDotDot )
{
    const auto result = grab::config::render_filename( dotDotPattern, fixedContext );

    expect_invalid_pattern( result, dotDotDetail );
}

TEST( ConfigPattern,
      MatchesPatternAcceptsRendered )
{
    const auto rendered = grab::config::render_filename( matchPattern, fixedContext );
    ASSERT_TRUE( rendered.has_value() ) << rendered.error().message;

    EXPECT_TRUE( grab::config::matches_pattern( matchPattern, *rendered ) );
}

TEST( ConfigPattern,
      MatchesPatternRejectsForeign )
{
    EXPECT_FALSE( grab::config::matches_pattern( matchPattern, foreignName ) );
    EXPECT_FALSE( grab::config::matches_pattern( matchPattern, invalidTimeName ) );
}
