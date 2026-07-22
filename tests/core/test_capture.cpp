#include "grab/capture.hpp"
#include "grab/ids.hpp"
#include "grab/image.hpp"
#include "grab/space.hpp"
#include "kernel/identity/id_factory.hpp"

#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

TEST( Capture,
      FrameCarriesProvenance )
{
    constexpr grab::CoordinateSpaceId outputSpace{ .value = 7U };
    constexpr grab::CaptureGeneration generation{ .value = 3U };
    constexpr std::int64_t            capturedAt = 1'234;
    constexpr std::uint32_t           width      = 2U;
    constexpr std::uint32_t           stride     = 8U;

    grab::Frame                       frame{
        .id = grab::detail::next_frame_id(),
        .image =
            {
                    .width  = width,
                    .height = 1U,
                    .stride = stride,
                    .format = grab::PixelFormat::Bgra,
                    .pixels = std::vector<std::byte>( stride ),
                    },
        .space          = outputSpace,
        .generation     = generation,
        .captured_at_ns = capturedAt,
        .content_rect   = {
                    .x     = 1.0,
                    .y     = static_cast<double>( width ),
                    .w     = static_cast<double>( width ),
                    .h     = 1.0,
                    .space = outputSpace,
                    },
    };

    EXPECT_NE( frame.id.value, 0U );
    EXPECT_EQ( frame.space, outputSpace );
    EXPECT_EQ( frame.generation, generation );
    EXPECT_EQ( frame.captured_at_ns, capturedAt );
    EXPECT_EQ( frame.content_rect.space, outputSpace );
    EXPECT_DOUBLE_EQ( frame.scale, 1.0 );
}

TEST( Capture,
      MatchEvidenceRoundTripsByValue )
{
    const grab::MatchEvidence original{
        .confidence = 0.75,
        .strategy   = "template",
        .region =
            {
                     .x     = 4.0,
                     .y     = 5.0,
                     .w     = 6.0,
                     .h     = 7.0,
                     .space = { .value = 8U },
                     },
        .target_offset_x      = 1.5,
        .target_offset_y      = -2.5,
        .source_frame         = { .value = 9U },
        .transform_generation = 10U,
        .timestamp_ns         = 11,
    };

    const grab::MatchEvidence roundTrip = []( grab::MatchEvidence value )
    {
        return value;
    }( original );
    EXPECT_DOUBLE_EQ( roundTrip.confidence, original.confidence );
    EXPECT_EQ( roundTrip.strategy, original.strategy );
    EXPECT_DOUBLE_EQ( roundTrip.region.x, original.region.x );
    EXPECT_EQ( roundTrip.region.space, original.region.space );
    EXPECT_DOUBLE_EQ( roundTrip.target_offset_x, original.target_offset_x );
    EXPECT_DOUBLE_EQ( roundTrip.target_offset_y, original.target_offset_y );
    EXPECT_EQ( roundTrip.source_frame, original.source_frame );
    EXPECT_EQ( roundTrip.transform_generation, original.transform_generation );
    EXPECT_EQ( roundTrip.timestamp_ns, original.timestamp_ns );
}

TEST( Capture,
      FrameIdsAreMonotonic )
{
    const auto first  = grab::detail::next_frame_id();
    const auto second = grab::detail::next_frame_id();

    EXPECT_LT( first.value, second.value );
}
