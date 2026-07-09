#include "grab/result.hpp"
#include "grab/session.hpp"
#include "session/record.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstdint>
#include <string>
// clang-format on

namespace
{

    constexpr std::uint16_t width   = 1'280U;
    constexpr std::uint16_t height  = 800U;
    constexpr std::int64_t  pid     = 4'242;
    constexpr std::uint64_t created = 99'999U;

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
            .mode              = grab::SessionMode::offscreen,
            .geometry          = geometry,
            .state             = grab::SessionState::ready,
            .supervisor_pid    = pid,
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
    EXPECT_EQ( parsed->mode, original.mode );
    EXPECT_EQ( parsed->state, original.state );
    EXPECT_EQ( parsed->geometry.width, width );
    EXPECT_EQ( parsed->supervisor_pid, pid );
    EXPECT_EQ( parsed->created_monotonic, created );
}

TEST( SessionRecord,
      RejectsMissingField )
{
    const auto parsed = grab::session::parse_record( R"({"name":"ai"})" );
    ASSERT_FALSE( parsed.has_value() );
    EXPECT_EQ( parsed.error().code, grab::ErrorCode::protocol_error );
}
