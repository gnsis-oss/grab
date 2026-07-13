#include "grab/pid.hpp"
#include "grab/result.hpp"
#include "grab/session.hpp"
#include "session/record.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstdint>
#include <nlohmann/json.hpp>    // IWYU pragma: keep
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>
// clang-format on

namespace
{

    constexpr std::uint16_t    width   = 1'280U;
    constexpr std::uint16_t    height  = 800U;
    constexpr std::int64_t     pid     = 4'242;
    constexpr std::uint64_t    created = 99'999U;
    constexpr std::string_view malformedNameMessage =
        "missing or malformed session record field: name";
    constexpr std::string_view malformedProviderMessage =
        "missing or malformed session record field: provider";
    constexpr std::string_view malformedWidthMessage =
        "missing or malformed session record field: width";
    constexpr std::string_view widthFieldName = "width";
    constexpr std::string_view widthAsText    = "1280";

    grab::session::SessionRecord
    sample_record()
    {
        constexpr grab::SessionGeometry geometry{
            .width  = width,
            .height = height,
        };

        return grab::session::SessionRecord{
            .name              = "ai",
            .provider          = "fake",
            .endpoint          = ":wl-7",
            .control_socket    = "/run/grab/ai.sock",
            .mode              = grab::SessionMode::Offscreen,
            .geometry          = geometry,
            .state             = grab::SessionState::Ready,
            .supervisor_pid    = grab::Pid{ pid },
            .created_monotonic = created,
        };
    }

}    // namespace

TEST( SessionRecord,
      JsonRoundTrips )
{
    const auto        original = sample_record();
    const std::string json     = grab::session::to_json( original );
    const auto        parsed   = grab::session::parse_record( json );

    ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;
    EXPECT_EQ( parsed->name, original.name );
    EXPECT_EQ( parsed->provider, original.provider );
    EXPECT_EQ( parsed->endpoint, original.endpoint );
    EXPECT_EQ( parsed->control_socket, original.control_socket );
    EXPECT_EQ( parsed->mode, original.mode );
    EXPECT_EQ( parsed->state, original.state );
    EXPECT_EQ( parsed->geometry.width, width );
    EXPECT_EQ( parsed->geometry.height, height );
    EXPECT_EQ( parsed->supervisor_pid, grab::Pid{ pid } );
    EXPECT_EQ( parsed->created_monotonic, created );
}

TEST( SessionRecord,
      EscapedStringsRoundTrip )
{
    auto original = sample_record();
    original.name = "session\b\f\n\r\t\"\\";

    const auto parsed =
        grab::session::parse_record( grab::session::to_json( original ) );

    ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;
    EXPECT_EQ( parsed->name, original.name );
}

TEST( SessionRecord,
      RejectsMissingField )
{
    const auto parsed = grab::session::parse_record( R"({"name":"ai"})" );
    ASSERT_FALSE( parsed.has_value() );
    EXPECT_EQ( parsed.error().code, grab::ErrorCode::ProtocolError );
    EXPECT_EQ( parsed.error().message, malformedProviderMessage );
}

TEST( SessionRecord,
      RejectsMalformedJson )
{
    const auto parsed = grab::session::parse_record( R"({"name":)" );

    ASSERT_FALSE( parsed.has_value() );
    EXPECT_EQ( parsed.error().code, grab::ErrorCode::ProtocolError );
    EXPECT_EQ( parsed.error().message, malformedNameMessage );
}

TEST( SessionRecord,
      RejectsWrongFieldType )
{
    auto object = nlohmann::json::parse( grab::session::to_json( sample_record() ) );
    object.at( widthFieldName ) = widthAsText;

    const auto parsed           = grab::session::parse_record( object.dump() );

    ASSERT_FALSE( parsed.has_value() );
    EXPECT_EQ( parsed.error().code, grab::ErrorCode::ProtocolError );
    EXPECT_EQ( parsed.error().message, malformedWidthMessage );
}
