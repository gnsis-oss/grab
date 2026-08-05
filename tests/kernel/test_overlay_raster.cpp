#include "codec/png.hpp"
#include "grab/geometry/rectangle.hpp"
#include "grab/geometry/size.hpp"
#include "grab/image.hpp"
#include "grab/overlay.hpp"
#include "grab/result.hpp"
#include "grab/space.hpp"
#include "kernel/presentation/overlay_raster.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace
{

    using grab::kernel::presentation::OverlayRaster;

    constexpr grab::geometry::Size goldenSurface{
        .width  = 64U,
        .height = 48U,
    };
    constexpr grab::geometry::Size damageSurface{
        .width  = 40U,
        .height = 32U,
    };
    constexpr grab::CoordinateSpaceId   surfaceSpace{ 7U };
    constexpr grab::overlay::SceneEpoch sceneEpoch{ .value = 3U };
    constexpr std::chrono::milliseconds startedAt{ 100 };
    constexpr std::chrono::milliseconds fadeDuration{ 1'000 };
    constexpr std::chrono::milliseconds ttlDuration{ 500 };
    constexpr std::chrono::milliseconds halfFadeAt   = startedAt + fadeDuration / 2;
    constexpr std::chrono::milliseconds fadeDeadline = startedAt + fadeDuration;
    constexpr std::chrono::milliseconds beforeTtlDeadline =
        startedAt + ttlDuration - std::chrono::milliseconds{ 1 };
    constexpr std::chrono::milliseconds ttlDeadline       = startedAt + ttlDuration;
    constexpr std::uint32_t             bgraBytesPerPixel = 4U;
    constexpr std::uint32_t             firstSlot         = 11U;
    constexpr std::uint32_t             secondSlot        = 12U;
    constexpr std::uint8_t              opaqueChannel     = 255U;
    constexpr std::uint8_t              transparentChannel{};
    // Coverage AA is deterministic, but PNG goldens allow +/-2 per channel so
    // harmless rounding differences between supported toolchains remain portable.
    constexpr int                       goldenTolerance   = 2;
    constexpr double                    rectangleX        = 10.0;
    constexpr double                    rectangleY        = 8.0;
    constexpr double                    rectangleWidth    = 8.0;
    constexpr double                    rectangleHeight   = 6.0;
    constexpr float                     damageStrokeWidth = 2.0F;
    constexpr std::int32_t              damagedX          = 7;
    constexpr std::int32_t              damagedY          = 5;
    constexpr std::uint32_t             damagedWidth      = 14U;
    constexpr std::uint32_t             damagedHeight     = 12U;
    constexpr double                    movedRectangleX   = 20.0;
    constexpr std::uint32_t             movedDamageWidth  = 24U;
    constexpr std::int32_t              centerPixelX      = 12;
    constexpr std::int32_t              centerPixelY      = 10;
    constexpr std::size_t               blueByteOffset{};
    constexpr std::size_t               greenByteOffset          = 1U;
    constexpr std::size_t               redByteOffset            = 2U;
    constexpr std::size_t               alphaByteOffset          = 3U;
    constexpr std::uint8_t              fadeRed                  = 200U;
    constexpr std::uint8_t              fadeGreen                = 100U;
    constexpr std::uint8_t              fadeBlue                 = 50U;
    constexpr std::uint8_t              fadeAlpha                = 240U;
    constexpr std::uint8_t              halfFadeAlpha            = 120U;
    constexpr std::size_t               singleDamageRect         = 1U;

    constexpr std::uint32_t             OverlapTestSurfaceWidth  = 48U;
    constexpr std::uint32_t             OverlapTestSurfaceHeight = 36U;
    constexpr double                    FirstShapeX              = 6.0;
    constexpr double                    FirstShapeY              = 6.0;
    constexpr double                    FirstShapeWidth          = 18.0;
    constexpr double                    FirstShapeHeight         = 14.0;
    constexpr std::uint8_t              FirstShapeRed            = 220U;
    constexpr std::uint8_t              FirstShapeGreen          = 40U;
    constexpr std::uint8_t              FirstShapeBlue           = 80U;
    constexpr std::uint8_t              FirstShapeAlpha          = 144U;
    constexpr double                    SecondShapeX             = 14.0;
    constexpr double                    SecondShapeY             = 10.0;
    constexpr double                    SecondShapeWidth         = 18.0;
    constexpr double                    SecondShapeHeight        = 14.0;
    constexpr std::uint8_t              SecondShapeRed           = 40U;
    constexpr std::uint8_t              SecondShapeGreen         = 120U;
    constexpr std::uint8_t              SecondShapeBlue          = 230U;
    constexpr std::uint8_t              SecondShapeAlpha         = 176U;
    constexpr grab::geometry::Size      OverlapTestSurface{
        .width  = OverlapTestSurfaceWidth,
        .height = OverlapTestSurfaceHeight,
    };
    constexpr grab::SpaceRect FirstShapeBounds{
        .x     = FirstShapeX,
        .y     = FirstShapeY,
        .w     = FirstShapeWidth,
        .h     = FirstShapeHeight,
        .space = surfaceSpace,
    };
    constexpr grab::SpaceRect SecondShapeBounds{
        .x     = SecondShapeX,
        .y     = SecondShapeY,
        .w     = SecondShapeWidth,
        .h     = SecondShapeHeight,
        .space = surfaceSpace,
    };
    constexpr grab::geometry::Rectangle OverlapFullSurfaceDamage{
        .width  = OverlapTestSurfaceWidth,
        .height = OverlapTestSurfaceHeight,
    };
    constexpr std::array OverlapExpectedDamage{
        grab::geometry::Rectangle{ .x = 5,  .y = 5, .width = 20U, .height = 16U},
        grab::geometry::Rectangle{.x = 13, .y = 21, .width = 20U,  .height = 4U},
        grab::geometry::Rectangle{.x = 25,  .y = 9,  .width = 8U, .height = 12U},
    };
    constexpr std::uint32_t        RetentionTestSurfaceWidth  = 64U;
    constexpr std::uint32_t        RetentionTestSurfaceHeight = 32U;
    constexpr double               UnchangedShapeX            = 46.0;
    constexpr double               UnchangedShapeY            = 8.0;
    constexpr double               UnchangedShapeWidth        = 10.0;
    constexpr double               UnchangedShapeHeight       = 10.0;
    constexpr std::uint8_t         UnchangedShapeRed          = 25U;
    constexpr std::uint8_t         UnchangedShapeGreen        = 210U;
    constexpr std::uint8_t         UnchangedShapeBlue         = 90U;
    constexpr std::uint8_t         UnchangedShapeAlpha        = 137U;
    constexpr double               MovingShapeInitialX        = 5.0;
    constexpr double               MovingShapeInitialY        = 8.0;
    constexpr double               MovingShapeWidth           = 10.0;
    constexpr double               MovingShapeHeight          = 10.0;
    constexpr double               MovingShapeNewX            = 24.0;
    constexpr std::uint8_t         MovingShapeRed             = 240U;
    constexpr std::uint8_t         MovingShapeGreen           = 120U;
    constexpr std::uint8_t         MovingShapeBlue            = 20U;
    constexpr std::uint8_t         MovingShapeAlpha           = 193U;
    constexpr grab::geometry::Size RetentionTestSurface{
        .width  = RetentionTestSurfaceWidth,
        .height = RetentionTestSurfaceHeight,
    };
    constexpr grab::SpaceRect UnchangedShapeBounds{
        .x     = UnchangedShapeX,
        .y     = UnchangedShapeY,
        .w     = UnchangedShapeWidth,
        .h     = UnchangedShapeHeight,
        .space = surfaceSpace,
    };
    constexpr grab::SpaceRect MovingShapeInitialBounds{
        .x     = MovingShapeInitialX,
        .y     = MovingShapeInitialY,
        .w     = MovingShapeWidth,
        .h     = MovingShapeHeight,
        .space = surfaceSpace,
    };
    constexpr grab::geometry::Rectangle RetentionFullSurfaceDamage{
        .width  = RetentionTestSurfaceWidth,
        .height = RetentionTestSurfaceHeight,
    };
    constexpr grab::geometry::Rectangle MovingShapeExpectedDamage{
        .x      = 4,
        .y      = 7,
        .width  = 31U,
        .height = 12U,
    };
    constexpr grab::geometry::Rectangle UnchangedShapeDamageBounds{
        .x      = 45,
        .y      = 7,
        .width  = 12U,
        .height = 12U,
    };
    constexpr grab::geometry::Rectangle UnchangedShapePixelRegion{
        .x      = 46,
        .y      = 8,
        .width  = 10U,
        .height = 10U,
    };
    constexpr auto updateGoldenEnvironment       = std::to_array( "GRAB_UPDATE_GOLDEN" );
    constexpr std::string_view updateGoldenValue = "1";
    constexpr grab::SpaceRect  goldenRectangleBounds{
        .x     = 10.25,
        .y     = 8.5,
        .w     = 28.0,
        .h     = 18.0,
        .space = surfaceSpace,
    };
    constexpr grab::overlay::StrokeStyle goldenRectangleStroke{
        .color =
            grab::overlay::Color{
                                 .r = 238U,
                                 .g = 61U,
                                 .b = 48U,
                                 .a = 230U,
                                 },
        .width_px = 3.5F,
    };
    constexpr grab::overlay::Ellipse goldenEllipseGeometry{
        .center =
            grab::SpacePoint{
                             .x     = 32.0,
                             .y     = 24.0,
                             .space = surfaceSpace,
                             },
        .radius_x = 17.25,
        .radius_y = 10.5,
    };
    constexpr grab::overlay::FillStyle goldenEllipseFill{
        .color = grab::overlay::Color{
                                      .r = 34U,
                                      .g = 197U,
                                      .b = 94U,
                                      .a = 160U,
                                      },
    };
    constexpr grab::overlay::StrokeStyle goldenEllipseStroke{
        .color =
            grab::overlay::Color{
                                 .r = 37U,
                                 .g = 99U,
                                 .b = 235U,
                                 },
        .width_px = 2.75F,
    };
    constexpr grab::SpacePoint goldenBezierStart{
        .x     = 6.0,
        .y     = 38.0,
        .space = surfaceSpace,
    };
    constexpr std::array goldenBezierControlPoints{
        grab::SpacePoint{.x = 16.0,  .y = 4.0, .space = surfaceSpace},
        grab::SpacePoint{.x = 46.0, .y = 44.0, .space = surfaceSpace},
        grab::SpacePoint{.x = 58.0, .y = 10.0, .space = surfaceSpace},
    };
    constexpr grab::overlay::StrokeStyle goldenBezierStroke{
        .color =
            grab::overlay::Color{
                                 .r = 249U,
                                 .g = 115U,
                                 .b = 22U,
                                 },
        .width_px = 4.0F,
    };
    constexpr std::array goldenPolygonPoints{
        grab::SpacePoint{.x = 32.0,  .y = 4.0, .space = surfaceSpace},
        grab::SpacePoint{.x = 40.0, .y = 39.0, .space = surfaceSpace},
        grab::SpacePoint{ .x = 8.0, .y = 16.0, .space = surfaceSpace},
        grab::SpacePoint{.x = 56.0, .y = 16.0, .space = surfaceSpace},
        grab::SpacePoint{.x = 24.0, .y = 39.0, .space = surfaceSpace},
    };
    constexpr grab::overlay::FillStyle goldenPolygonFill{
        .color = grab::overlay::Color{
                                      .r = 168U,
                                      .g = 85U,
                                      .b = 247U,
                                      .a = 210U,
                                      },
    };
    constexpr grab::overlay::StrokeStyle goldenPolygonStroke{
        .color =
            grab::overlay::Color{
                                 .r = 241U,
                                 .g = 245U,
                                 .b = 249U,
                                 },
        .width_px = 1.5F,
    };
    constexpr grab::overlay::Ellipse zeroRadiusEllipseGeometry{
        .center =
            grab::SpacePoint{
                             .x     = 20.0,
                             .y     = 20.0,
                             .space = surfaceSpace,
                             },
        .radius_x = 0.0,
        .radius_y = 8.0,
    };

    [[nodiscard]]
    grab::overlay::Color
    color( std::uint8_t red,
           std::uint8_t green,
           std::uint8_t blue,
           std::uint8_t alpha = opaqueChannel )
    {
        return grab::overlay::Color{
            .r = red,
            .g = green,
            .b = blue,
            .a = alpha,
        };
    }

    [[nodiscard]]
    grab::overlay::Shape
    filled_rectangle_shape( grab::SpaceRect      bounds,
                            grab::overlay::Color fill_color )
    {
        grab::overlay::Shape shape;
        shape.geometry = grab::overlay::Rect{ .bounds = bounds };
        shape.fill     = grab::overlay::FillStyle{
            .color = fill_color,
        };
        return shape;
    }

    [[nodiscard]]
    grab::overlay::ShapeRecord
    record( grab::overlay::Shape      shape,
            std::uint32_t             slot  = firstSlot,
            std::chrono::milliseconds began = startedAt )
    {
        return grab::overlay::ShapeRecord{
            .id =
                grab::overlay::ShapeId{
                                       .epoch = sceneEpoch,
                                       .slot  = slot,
                                       },
            .shape      = std::move( shape ),
            .started_at = began,
        };
    }

    [[nodiscard]]
    grab::overlay::Shape
    damage_rect_shape()
    {
        grab::overlay::Shape shape;
        shape.geometry = grab::overlay::Rect{
            .bounds = grab::SpaceRect{
                                      .x     = rectangleX,
                                      .y     = rectangleY,
                                      .w     = rectangleWidth,
                                      .h     = rectangleHeight,
                                      .space = surfaceSpace,
                                      },
        };
        shape.stroke = grab::overlay::StrokeStyle{
            .color    = color( opaqueChannel, transparentChannel, transparentChannel ),
            .width_px = damageStrokeWidth,
        };
        shape.fill = grab::overlay::FillStyle{
            .color = color( transparentChannel, opaqueChannel, transparentChannel ),
        };
        return shape;
    }

    [[nodiscard]]
    grab::overlay::Shape
    fade_shape()
    {
        auto shape   = damage_rect_shape();
        shape.stroke = std::nullopt;
        shape.fill   = grab::overlay::FillStyle{
            .color = color( fadeRed, fadeGreen, fadeBlue, fadeAlpha ),
        };
        shape.lifetime = grab::overlay::Fade{ .duration = fadeDuration };
        return shape;
    }

    [[nodiscard]]
    grab::overlay::Shape
    golden_stroked_rect()
    {
        grab::overlay::Shape shape;
        shape.geometry = grab::overlay::Rect{
            .bounds = goldenRectangleBounds,
        };
        shape.stroke = goldenRectangleStroke;
        return shape;
    }

    [[nodiscard]]
    grab::overlay::Shape
    golden_ellipse()
    {
        grab::overlay::Shape shape;
        shape.geometry = goldenEllipseGeometry;
        shape.fill     = goldenEllipseFill;
        shape.stroke   = goldenEllipseStroke;
        return shape;
    }

    [[nodiscard]]
    grab::overlay::Shape
    golden_bezier()
    {
        grab::overlay::Path path;
        path.commands = {
            grab::overlay::MoveTo{ .point = goldenBezierStart },
            grab::overlay::BezierTo{
                                  .control = std::vector<grab::SpacePoint>{
                    goldenBezierControlPoints.begin(),
                    goldenBezierControlPoints.end(),
                }, },
        };

        grab::overlay::Shape shape;
        shape.geometry = std::move( path );
        shape.stroke   = goldenBezierStroke;
        return shape;
    }

    [[nodiscard]]
    grab::overlay::Shape
    golden_polygon()
    {
        grab::overlay::Shape shape;
        shape.geometry = grab::overlay::Polygon{
            .points = std::vector<grab::SpacePoint>{
                                                    goldenPolygonPoints.begin(),
                                                    goldenPolygonPoints.end(),
                                                    },
        };
        shape.fill   = goldenPolygonFill;
        shape.stroke = goldenPolygonStroke;
        return shape;
    }

    [[nodiscard]]
    std::filesystem::path
    golden_path( std::string_view filename )
    {
        return std::filesystem::path{ GRAB_OVERLAY_GOLDEN_DIR } /
               std::string{ filename };
    }

    [[nodiscard]]
    bool
    updating_goldens()
    {
        // Tests only read the process environment established before gtest starts.
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        const auto* value = std::getenv( updateGoldenEnvironment.data() );
        return value != nullptr && std::string_view{ value } == updateGoldenValue;
    }

    [[nodiscard]]
    std::vector<std::byte>
    read_file( const std::filesystem::path& path )
    {
        std::ifstream     stream{ path, std::ios::binary };
        std::vector<char> bytes{
            std::istreambuf_iterator<char>{ stream },
            std::istreambuf_iterator<char>{}
        };
        std::vector<std::byte> result;
        result.reserve( bytes.size() );
        std::ranges::transform(
            bytes,
            std::back_inserter( result ),
            []( char value )
            {
                return static_cast<std::byte>( static_cast<unsigned char>( value ) );
            }
        );
        return result;
    }

    void
    write_file( const std::filesystem::path& path,
                std::span<const std::byte>   bytes )
    {
        std::filesystem::create_directories( path.parent_path() );
        std::ofstream stream{ path, std::ios::binary | std::ios::trunc };
        ASSERT_TRUE( stream.is_open() ) << path;
        for( const auto value : bytes )
        {
            stream.put( static_cast<char>( std::to_integer<unsigned char>( value ) ) );
        }
        ASSERT_TRUE( stream.good() ) << path;
    }

    void
    expect_image_near( const grab::Image& actual,
                       const grab::Image& expected )
    {
        ASSERT_EQ( actual.width, expected.width );
        ASSERT_EQ( actual.height, expected.height );
        ASSERT_EQ( actual.stride, expected.stride );
        ASSERT_EQ( actual.format, expected.format );
        ASSERT_EQ( actual.pixels.size(), expected.pixels.size() );
        for( std::size_t index{}; index < actual.pixels.size(); ++index )
        {
            const auto actual_channel = static_cast<int>(
                std::to_integer<unsigned char>( actual.pixels.at( index ) )
            );
            const auto expected_channel = static_cast<int>(
                std::to_integer<unsigned char>( expected.pixels.at( index ) )
            );
            EXPECT_LE( std::abs( actual_channel - expected_channel ), goldenTolerance )
                << "channel byte " << index;
        }
    }

    void
    expect_golden( const grab::Image& image,
                   std::string_view   filename )
    {
        const auto encoded = grab::codec::encode_png( image );
        ASSERT_TRUE( encoded.has_value() ) << encoded.error().message;
        const auto path = golden_path( filename );
        if( updating_goldens() )
        {
            write_file( path, *encoded );
        }
        ASSERT_TRUE( std::filesystem::exists( path ) )
            << path << " is missing; run with GRAB_UPDATE_GOLDEN=1";

        const auto actual = grab::codec::decode_png( *encoded );
        ASSERT_TRUE( actual.has_value() ) << actual.error().message;
        const auto expected = grab::codec::decode_png( read_file( path ) );
        ASSERT_TRUE( expected.has_value() ) << expected.error().message;
        expect_image_near( *actual, *expected );
    }

    [[nodiscard]]
    const std::byte&
    pixel_channel( const grab::Image& image,
                   std::int32_t       x,
                   std::int32_t       y,
                   std::size_t        channel )
    {
        const auto row_offset   = static_cast<std::size_t>( y ) * image.stride;
        const auto pixel_offset = static_cast<std::size_t>( x ) * bgraBytesPerPixel;
        const auto offset       = row_offset + pixel_offset + channel;
        return image.pixels.at( offset );
    }

    [[nodiscard]]
    unsigned char
    channel_value( const grab::Image& image,
                   std::int32_t       x,
                   std::int32_t       y,
                   std::size_t        channel )
    {
        return std::to_integer<unsigned char>( pixel_channel( image, x, y, channel ) );
    }

    [[nodiscard]]
    std::vector<std::byte>
    copy_pixel_region( std::span<const std::byte> pixels,
                       std::uint32_t              stride,
                       grab::geometry::Rectangle  region )
    {
        const auto             first_x   = static_cast<std::size_t>( region.x );
        const auto             first_y   = static_cast<std::size_t>( region.y );
        const auto             row_bytes = static_cast<std::size_t>( region.width ) *
                                           static_cast<std::size_t>( bgraBytesPerPixel );
        const auto             row_count = static_cast<std::size_t>( region.height );

        std::vector<std::byte> copied;
        copied.reserve( row_bytes * row_count );
        for( std::size_t row{}; row < row_count; ++row )
        {
            const auto offset =
                ( ( first_y + row ) * static_cast<std::size_t>( stride ) ) +
                ( first_x * static_cast<std::size_t>( bgraBytesPerPixel ) );
            const auto source = pixels.subspan( offset, row_bytes );
            copied.insert( copied.end(), source.begin(), source.end() );
        }
        return copied;
    }

    [[nodiscard]]
    bool
    rectangles_intersect( grab::geometry::Rectangle left,
                          grab::geometry::Rectangle right )
    {
        return left.x <
               right.right() &&
               right.x <
               left.right() &&
               left.y <
               right.bottom() &&
               right.y < left.bottom();
    }

    void
    expect_damage( std::span<const grab::geometry::Rectangle> actual,
                   std::span<const grab::geometry::Rectangle> expected )
    {
        ASSERT_EQ( actual.size(), expected.size() );
        auto        expected_rectangle = expected.begin();
        std::size_t index{};
        for( const auto rectangle : actual )
        {
            EXPECT_EQ( rectangle, *expected_rectangle ) << "damage rectangle " << index;
            ++expected_rectangle;
            ++index;
        }
    }

    void
    expect_all_transparent( const grab::Image& image )
    {
        EXPECT_TRUE( std::ranges::all_of( image.pixels,
                                          []( std::byte value )
                                          {
                                              return value == std::byte{};
                                          } ) );
    }

    [[nodiscard]]
    grab::geometry::Rectangle
    expected_damage_rect()
    {
        return grab::geometry::Rectangle{
            .x      = damagedX,
            .y      = damagedY,
            .width  = damagedWidth,
            .height = damagedHeight,
        };
    }

}    // namespace

TEST( OverlayRaster,
      CreatesPremultipliedBgraFrameAndDamagesFullSurfaceFirst )
{
    auto raster = OverlayRaster::create( damageSurface );
    ASSERT_TRUE( raster.has_value() ) << raster.error().message;

    const std::array shapes{ record( fade_shape() ) };
    const auto       rendered = raster->render( shapes, startedAt );

    ASSERT_TRUE( rendered.has_value() ) << rendered.error().message;
    EXPECT_EQ( raster->size(), damageSurface );
    EXPECT_EQ( rendered->pixels.format, grab::PixelFormat::Bgra );
    EXPECT_EQ( rendered->pixels.width, damageSurface.width );
    EXPECT_EQ( rendered->pixels.height, damageSurface.height );
    EXPECT_EQ( rendered->pixels.stride, damageSurface.width * bgraBytesPerPixel );
    ASSERT_EQ( rendered->damage.size(), singleDamageRect );
    const grab::geometry::Rectangle full_surface_damage{
        .width  = damageSurface.width,
        .height = damageSurface.height,
    };
    EXPECT_EQ( rendered->damage.front(), full_surface_damage );

    EXPECT_EQ(
        channel_value( rendered->pixels, centerPixelX, centerPixelY, alphaByteOffset ),
        fadeAlpha
    );
    EXPECT_LT(
        channel_value( rendered->pixels, centerPixelX, centerPixelY, redByteOffset ),
        fadeRed
    );
    EXPECT_LT(
        channel_value( rendered->pixels, centerPixelX, centerPixelY, greenByteOffset ),
        fadeGreen
    );
    EXPECT_LT(
        channel_value( rendered->pixels, centerPixelX, centerPixelY, blueByteOffset ),
        fadeBlue
    );
}

TEST( OverlayRaster,
      RemovalClearsPixelsAndDamagesPreviousBoundsExactly )
{
    auto raster = OverlayRaster::create( damageSurface );
    ASSERT_TRUE( raster.has_value() );
    const std::array shapes{ record( damage_rect_shape() ) };
    ASSERT_TRUE( raster->render( shapes, startedAt ).has_value() );

    const auto removed = raster->render( {}, startedAt );

    ASSERT_TRUE( removed.has_value() ) << removed.error().message;
    ASSERT_EQ( removed->damage.size(), singleDamageRect );
    EXPECT_EQ( removed->damage.front(), expected_damage_rect() );
    expect_all_transparent( removed->pixels );

    const auto stable = raster->render( {}, startedAt );
    ASSERT_TRUE( stable.has_value() );
    EXPECT_TRUE( stable->damage.empty() );
}

TEST( OverlayRaster,
      MovingShapeDamagesUnionOfOldAndNewBounds )
{
    auto raster = OverlayRaster::create( damageSurface );
    ASSERT_TRUE( raster.has_value() );
    auto             shape = damage_rect_shape();
    const std::array initial{ record( shape ) };
    ASSERT_TRUE( raster->render( initial, startedAt ).has_value() );

    std::get<grab::overlay::Rect>( shape.geometry ).bounds.x = movedRectangleX;
    const std::array moved{ record( shape ) };
    const auto       rendered = raster->render( moved, startedAt );

    ASSERT_TRUE( rendered.has_value() );
    ASSERT_EQ( rendered->damage.size(), singleDamageRect );
    const grab::geometry::Rectangle moved_damage{
        .x      = damagedX,
        .y      = damagedY,
        .width  = movedDamageWidth,
        .height = damagedHeight,
    };
    EXPECT_EQ( rendered->damage.front(), moved_damage );
}

TEST( OverlayRaster,
      FadeChangesAlphaLinearlyThenExpiresWithoutGhosts )
{
    auto raster = OverlayRaster::create( damageSurface );
    ASSERT_TRUE( raster.has_value() );
    const std::array shapes{ record( fade_shape() ) };
    ASSERT_TRUE( raster->render( shapes, startedAt ).has_value() );

    const auto halfway = raster->render( shapes, halfFadeAt );
    ASSERT_TRUE( halfway.has_value() );
    ASSERT_EQ( halfway->damage.size(), singleDamageRect );
    EXPECT_EQ(
        channel_value( halfway->pixels, centerPixelX, centerPixelY, alphaByteOffset ),
        halfFadeAlpha
    );

    const auto expired = raster->render( shapes, fadeDeadline );
    ASSERT_TRUE( expired.has_value() );
    ASSERT_EQ( expired->damage.size(), singleDamageRect );
    expect_all_transparent( expired->pixels );

    const auto still_expired = raster->render( shapes, fadeDeadline );
    ASSERT_TRUE( still_expired.has_value() );
    EXPECT_TRUE( still_expired->damage.empty() );
}

TEST( OverlayRaster,
      TtlStaysFullyOpaqueUntilDeadlineThenExpires )
{
    auto shape     = fade_shape();
    shape.lifetime = grab::overlay::Ttl{ .duration = ttlDuration };
    const std::array shapes{ record( shape ) };
    auto             raster = OverlayRaster::create( damageSurface );
    ASSERT_TRUE( raster.has_value() );
    ASSERT_TRUE( raster->render( shapes, startedAt ).has_value() );

    const auto before_deadline = raster->render( shapes, beforeTtlDeadline );
    ASSERT_TRUE( before_deadline.has_value() );
    EXPECT_TRUE( before_deadline->damage.empty() );
    EXPECT_EQ( channel_value( before_deadline->pixels,
                              centerPixelX,
                              centerPixelY,
                              alphaByteOffset ),
               fadeAlpha );

    const auto expired = raster->render( shapes, ttlDeadline );
    ASSERT_TRUE( expired.has_value() );
    ASSERT_EQ( expired->damage.size(), singleDamageRect );
    expect_all_transparent( expired->pixels );
}

TEST( OverlayRaster,
      ZeroPointPolygonAndZeroRadiusEllipseRenderNothing )
{
    grab::overlay::Shape polygon;
    polygon.geometry = grab::overlay::Polygon{};
    polygon.fill.emplace();

    grab::overlay::Shape ellipse;
    ellipse.geometry = zeroRadiusEllipseGeometry;
    ellipse.fill.emplace();
    ellipse.stroke.emplace();

    const std::array shapes{
        record( polygon, firstSlot ),
        record( ellipse, secondSlot ),
    };
    auto raster = OverlayRaster::create( damageSurface );
    ASSERT_TRUE( raster.has_value() );

    const auto rendered = raster->render( shapes, startedAt );

    ASSERT_TRUE( rendered.has_value() );
    expect_all_transparent( rendered->pixels );
    const auto stable = raster->render( shapes, startedAt );
    ASSERT_TRUE( stable.has_value() );
    EXPECT_TRUE( stable->damage.empty() );
}

TEST( OverlayRaster,
      DrawsRecordsInCallerOrder )
{
    auto bottom   = damage_rect_shape();
    bottom.stroke = std::nullopt;
    bottom.fill   = grab::overlay::FillStyle{
        .color = color( opaqueChannel, transparentChannel, transparentChannel ),
    };
    bottom.z = -1;

    auto top = bottom;
    top.fill = grab::overlay::FillStyle{
        .color = color( transparentChannel, transparentChannel, opaqueChannel ),
    };
    top.z = 1;
    const std::array shapes{
        record( bottom, firstSlot ),
        record( top, secondSlot ),
    };
    auto raster = OverlayRaster::create( damageSurface );
    ASSERT_TRUE( raster.has_value() );

    const auto rendered = raster->render( shapes, startedAt );

    ASSERT_TRUE( rendered.has_value() );
    EXPECT_EQ(
        channel_value( rendered->pixels, centerPixelX, centerPixelY, blueByteOffset ),
        opaqueChannel
    );
    EXPECT_EQ(
        channel_value( rendered->pixels, centerPixelX, centerPixelY, redByteOffset ),
        transparentChannel
    );
}

TEST( OverlayRaster,
      OverlappingTranslucentShapesWithOverlappingDamage )
{
    auto first_shape = filled_rectangle_shape(
        FirstShapeBounds,
        color( FirstShapeRed, FirstShapeGreen, FirstShapeBlue, FirstShapeAlpha )
    );
    first_shape.lifetime = grab::overlay::Fade{ .duration = fadeDuration };
    auto second_shape    = filled_rectangle_shape(
        SecondShapeBounds,
        color( SecondShapeRed, SecondShapeGreen, SecondShapeBlue, SecondShapeAlpha )
    );
    second_shape.lifetime = grab::overlay::Fade{ .duration = fadeDuration };
    const std::array shapes{
        record( first_shape, firstSlot ),
        record( second_shape, secondSlot ),
    };

    auto raster = OverlayRaster::create( OverlapTestSurface );
    ASSERT_TRUE( raster.has_value() ) << raster.error().message;
    const auto initial_frame = raster->render( shapes, startedAt );
    ASSERT_TRUE( initial_frame.has_value() ) << initial_frame.error().message;
    ASSERT_EQ( initial_frame->damage.size(), singleDamageRect );
    EXPECT_EQ( initial_frame->damage.front(), OverlapFullSurfaceDamage );

    // Advancing both fades changes both shapes and produces overlapping raw damage.
    const auto damaged_frame = raster->render( shapes, halfFadeAt );
    ASSERT_TRUE( damaged_frame.has_value() ) << damaged_frame.error().message;
    expect_damage( damaged_frame->damage, OverlapExpectedDamage );

    auto reference_raster = OverlayRaster::create( OverlapTestSurface );
    ASSERT_TRUE( reference_raster.has_value() ) << reference_raster.error().message;
    const auto reference_frame = reference_raster->render( shapes, halfFadeAt );
    ASSERT_TRUE( reference_frame.has_value() ) << reference_frame.error().message;
    ASSERT_EQ( reference_frame->damage.size(), singleDamageRect );
    EXPECT_EQ( reference_frame->damage.front(), OverlapFullSurfaceDamage );

    EXPECT_EQ( damaged_frame->pixels.pixels, reference_frame->pixels.pixels );
}

TEST( OverlayRaster,
      RetainedPixelsForUntouchedShapesAcrossRenders )
{
    auto moving_shape = filled_rectangle_shape(
        MovingShapeInitialBounds,
        color( MovingShapeRed, MovingShapeGreen, MovingShapeBlue, MovingShapeAlpha )
    );
    const auto unchanged_shape = filled_rectangle_shape( UnchangedShapeBounds,
                                                         color( UnchangedShapeRed,
                                                                UnchangedShapeGreen,
                                                                UnchangedShapeBlue,
                                                                UnchangedShapeAlpha ) );
    const std::array initial_shapes{
        record( moving_shape, firstSlot ),
        record( unchanged_shape, secondSlot ),
    };

    auto raster = OverlayRaster::create( RetentionTestSurface );
    ASSERT_TRUE( raster.has_value() ) << raster.error().message;
    const auto initial_frame = raster->render( initial_shapes, startedAt );
    ASSERT_TRUE( initial_frame.has_value() ) << initial_frame.error().message;
    ASSERT_EQ( initial_frame->damage.size(), singleDamageRect );
    EXPECT_EQ( initial_frame->damage.front(), RetentionFullSurfaceDamage );
    const auto frame1_stride = initial_frame->pixels.stride;
    const auto frame1_pixels = initial_frame->pixels.pixels;

    std::get<grab::overlay::Rect>( moving_shape.geometry ).bounds.x = MovingShapeNewX;
    const std::array updated_shapes{
        record( moving_shape, firstSlot ),
        record( unchanged_shape, secondSlot ),
    };
    const auto rendered = raster->render( updated_shapes, startedAt );
    ASSERT_TRUE( rendered.has_value() ) << rendered.error().message;
    ASSERT_EQ( rendered->damage.size(), singleDamageRect );
    EXPECT_EQ( rendered->damage.front(), MovingShapeExpectedDamage );
    EXPECT_TRUE( std::ranges::none_of(
        rendered->damage,
        []( grab::geometry::Rectangle damage )
        {
            return rectangles_intersect( damage, UnchangedShapeDamageBounds );
        }
    ) );

    const auto frame1_region = copy_pixel_region( std::span{ frame1_pixels },
                                                  frame1_stride,
                                                  UnchangedShapePixelRegion );
    const auto frame2_region = copy_pixel_region( std::span{ rendered->pixels.pixels },
                                                  rendered->pixels.stride,
                                                  UnchangedShapePixelRegion );
    EXPECT_TRUE( std::ranges::equal( frame1_region, frame2_region ) );
}

TEST( OverlayRasterGolden,
      StrokedRectangle )
{
    auto raster = OverlayRaster::create( goldenSurface );
    ASSERT_TRUE( raster.has_value() );
    const std::array shapes{ record( golden_stroked_rect() ) };
    const auto       frame = raster->render( shapes, startedAt );
    ASSERT_TRUE( frame.has_value() );
    expect_golden( frame->pixels, "stroked_rect.png" );
}

TEST( OverlayRasterGolden,
      FilledAndStrokedEllipse )
{
    auto raster = OverlayRaster::create( goldenSurface );
    ASSERT_TRUE( raster.has_value() );
    const std::array shapes{ record( golden_ellipse() ) };
    const auto       frame = raster->render( shapes, startedAt );
    ASSERT_TRUE( frame.has_value() );
    expect_golden( frame->pixels, "filled_stroked_ellipse.png" );
}

TEST( OverlayRasterGolden,
      OpenBezierPath )
{
    auto raster = OverlayRaster::create( goldenSurface );
    ASSERT_TRUE( raster.has_value() );
    const std::array shapes{ record( golden_bezier() ) };
    const auto       frame = raster->render( shapes, startedAt );
    ASSERT_TRUE( frame.has_value() );
    expect_golden( frame->pixels, "open_bezier.png" );
}

TEST( OverlayRasterGolden,
      PolygonUsesNonzeroWinding )
{
    auto raster = OverlayRaster::create( goldenSurface );
    ASSERT_TRUE( raster.has_value() );
    const std::array shapes{ record( golden_polygon() ) };
    const auto       frame = raster->render( shapes, startedAt );
    ASSERT_TRUE( frame.has_value() );
    EXPECT_GT( channel_value( frame->pixels,
                              static_cast<std::int32_t>( goldenSurface.width / 2U ),
                              static_cast<std::int32_t>( goldenSurface.height / 2U ),
                              alphaByteOffset ),
               transparentChannel );
    expect_golden( frame->pixels, "polygon_nonzero.png" );
}

TEST( OverlayRasterGolden,
      FadeAtStart )
{
    auto raster = OverlayRaster::create( goldenSurface );
    ASSERT_TRUE( raster.has_value() );
    const std::array shapes{ record( fade_shape() ) };
    const auto       frame = raster->render( shapes, startedAt );
    ASSERT_TRUE( frame.has_value() );
    expect_golden( frame->pixels, "fade_start.png" );
}

TEST( OverlayRasterGolden,
      FadeAtHalfDuration )
{
    auto raster = OverlayRaster::create( goldenSurface );
    ASSERT_TRUE( raster.has_value() );
    const std::array shapes{ record( fade_shape() ) };
    const auto       frame = raster->render( shapes, halfFadeAt );
    ASSERT_TRUE( frame.has_value() );
    expect_golden( frame->pixels, "fade_half.png" );
}

TEST( OverlayRasterGolden,
      FadeAtExpiry )
{
    auto raster = OverlayRaster::create( goldenSurface );
    ASSERT_TRUE( raster.has_value() );
    const std::array shapes{ record( fade_shape() ) };
    const auto       frame = raster->render( shapes, fadeDeadline );
    ASSERT_TRUE( frame.has_value() );
    expect_all_transparent( frame->pixels );
    expect_golden( frame->pixels, "fade_expired.png" );
}

TEST( OverlayRaster,
      RejectsSurfaceWhoseFlatBufferWouldOverflow )
{
    constexpr grab::geometry::Size impossibleSurface{
        .width  = std::numeric_limits<std::uint32_t>::max(),
        .height = std::numeric_limits<std::uint32_t>::max(),
    };
    const auto raster = OverlayRaster::create( impossibleSurface );

    ASSERT_FALSE( raster.has_value() );
    EXPECT_EQ( raster.error().code, grab::ErrorCode::Overflowed );
}
