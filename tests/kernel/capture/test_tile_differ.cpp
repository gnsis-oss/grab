#include "grab/geometry/size.hpp"
#include "grab/image.hpp"
#include "kernel/capture/tile_differ.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstddef>
#include <cstdint>
// clang-format on

namespace
{

    [[nodiscard]]
    grab::Image
    image( std::uint32_t width,
           std::uint32_t height )
    {
        const auto stride = width * grab::bytes_per_pixel( grab::PixelFormat::Rgba );
        return grab::Image{
            .width  = width,
            .height = height,
            .stride = stride,
            .format = grab::PixelFormat::Rgba,
            .pixels =
                std::vector<std::byte>( static_cast<std::size_t>( stride ) * height ),
        };
    }

    void
    change_pixel( grab::Image&  value,
                  std::uint32_t x,
                  std::uint32_t y )
    {
        const auto offset    = ( static_cast<std::size_t>( y ) * value.stride ) +
                               ( x * grab::bytes_per_pixel( value.format ) );
        value.pixels[offset] = std::byte{ 0X7F };
    }

}    // namespace

TEST( TileDiffer,
      FindsExactlyChangedTilesIncludingClippedEdges )
{
    const auto previous = image( 5U, 3U );
    auto       current  = previous;
    change_pixel( current, 1U, 1U );
    change_pixel( current, 4U, 2U );

    const auto result = grab::kernel::TileDiffer{}.diff(
        previous,
        current,
        grab::geometry::Size{ .width = 2U, .height = 2U }
    );

    ASSERT_EQ( result.kind, grab::kernel::TileDiffKind::DirtyTiles );
    ASSERT_EQ( result.dirty_tiles.size(), 2U );
    const grab::geometry::Rectangle
        expected_first{ .x = 0, .y = 0, .width = 2U, .height = 2U };
    const grab::geometry::Rectangle
        expected_second{ .x = 4, .y = 2, .width = 1U, .height = 1U };
    EXPECT_EQ( result.dirty_tiles.at( 0 ), expected_first );
    EXPECT_EQ( result.dirty_tiles.at( 1 ), expected_second );
}

TEST( TileDiffer,
      ReportsNoChangeForIdenticalImages )
{
    const auto previous = image( 4U, 4U );
    const auto result   = grab::kernel::TileDiffer{}.diff(
        previous,
        previous,
        grab::geometry::Size{ .width = 2U, .height = 2U }
    );

    EXPECT_EQ( result.kind, grab::kernel::TileDiffKind::NoChange );
    EXPECT_TRUE( result.dirty_tiles.empty() );
}

TEST( TileDiffer,
      GeometryMismatchRequestsFullInvalidation )
{
    const auto result = grab::kernel::TileDiffer{}.diff(
        image( 4U, 4U ),
        image( 5U, 4U ),
        grab::geometry::Size{ .width = 2U, .height = 2U }
    );

    EXPECT_EQ( result.kind, grab::kernel::TileDiffKind::FullInvalidation );
    EXPECT_TRUE( result.dirty_tiles.empty() );
    EXPECT_FALSE( result.reason.empty() );
}
