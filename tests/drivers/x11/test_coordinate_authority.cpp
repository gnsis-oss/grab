#include "core/space_graph.hpp"    // NOLINT(misc-include-cleaner)
#include "drivers/desktop/x11/coordinate_authority.hpp"
#include "grab/result.hpp"
#include "screen/enumerate.hpp"

#include <array>
#include <cstdint>
#include <gtest/gtest.h>

namespace
{

    constexpr std::int32_t  outputOriginX = 1'600;
    constexpr std::int32_t  outputOriginY = -120;
    constexpr double        localX        = 25.0;
    constexpr double        localY        = 40.0;
    constexpr std::uint32_t outputWidth   = 1'920U;
    constexpr std::uint32_t outputHeight  = 1'080U;

    [[nodiscard]]
    grab::screen::OutputInfo
    output_at( std::int32_t x,
               std::int32_t y )
    {
        return grab::screen::OutputInfo{
            .name   = "DP-1",
            .bounds = {
                       .x      = x,
                       .y      = y,
                       .width  = outputWidth,
                       .height = outputHeight,
                       },
        };
    }

}    // namespace

TEST( CoordinateAuthority,
      RegistersOutputToGlobalTranslation )
{
    grab::drivers::desktop::x11::CoordinateAuthority authority;
    const std::array outputs{ output_at( outputOriginX, outputOriginY ) };
    ASSERT_TRUE( authority.refresh( outputs ).has_value() );

    const auto output = authority.output_space( "DP-1" );
    ASSERT_TRUE( output.has_value() );
    EXPECT_DOUBLE_EQ( output->scale, 1.0 );
    EXPECT_EQ( output->generation, authority.capture_generation() );
    EXPECT_NE( output->generation.value, 0U );

    const auto graph = authority.graph();
    ASSERT_NE( graph, nullptr );
    const auto mapped = graph->map( { .x = localX, .y = localY, .space = output->space },
                                    authority.global_space() );
    ASSERT_TRUE( mapped.has_value() );
    EXPECT_DOUBLE_EQ( mapped->x, static_cast<double>( outputOriginX ) + localX );
    EXPECT_DOUBLE_EQ( mapped->y, static_cast<double>( outputOriginY ) + localY );
    EXPECT_EQ( mapped->space, authority.global_space() );
}

TEST( CoordinateAuthority,
      TopologyChangeInvalidatesPriorGraphSnapshot )
{
    grab::drivers::desktop::x11::CoordinateAuthority authority;
    const std::array initial_outputs{ output_at( outputOriginX, outputOriginY ) };
    ASSERT_TRUE( authority.refresh( initial_outputs ).has_value() );

    const auto stale_graph  = authority.graph();
    const auto stale_global = authority.global_space();
    const auto stale_output = authority.output_space( "DP-1" );
    ASSERT_TRUE( stale_output.has_value() );
    const auto       initial_generation = authority.generation();

    const std::array moved_outputs{ output_at( outputOriginX + 1, outputOriginY ) };
    ASSERT_TRUE( authority.refresh( moved_outputs ).has_value() );
    EXPECT_EQ( authority.generation().value, initial_generation.value + 1U );

    const auto stale_mapping =
        stale_graph->map( { .x = localX, .y = localY, .space = stale_output->space },
                          stale_global );
    ASSERT_FALSE( stale_mapping.has_value() );
    EXPECT_EQ( stale_mapping.error().code, grab::ErrorCode::TopologyChanged );

    const auto fresh_output = authority.output_space( "DP-1" );
    ASSERT_TRUE( fresh_output.has_value() );
    const auto fresh_mapping = authority.graph()->map(
        { .x = localX, .y = localY, .space = fresh_output->space },
        authority.global_space()
    );
    ASSERT_TRUE( fresh_mapping.has_value() );
    EXPECT_DOUBLE_EQ( fresh_mapping->x,
                      static_cast<double>( outputOriginX + 1 ) + localX );
}
