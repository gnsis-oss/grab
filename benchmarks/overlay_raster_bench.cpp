// Overlay raster scaling study.
//
// `grab trail` and `grab sketch` are reported as laggy in two distinct
// regimes: a fast pointer, and a large shape. Those are different cost terms —
// one scales with the number of live shapes, the other with the number of
// covered pixels — and a single "the overlay is slow" number cannot separate
// them.
//
// This measures one dimension at a time against the real OverlayRaster, with
// no X server involved, so a change can be attributed to the code that
// changed. It is not a correctness test; tests/kernel/test_overlay_raster.cpp
// is.
//
// Run: build/dev/grab_overlay_raster_bench [--frames N] [--scenario NAME]

#include "grab/geometry/rectangle.hpp"
#include "grab/geometry/size.hpp"
#include "grab/overlay.hpp"
#include "grab/space.hpp"
#include "kernel/presentation/overlay_raster.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

    using grab::kernel::presentation::OverlayRaster;
    using Clock = std::chrono::steady_clock;

    // A 3200x2000 desktop: the surface the reported lag was measured on.
    constexpr std::uint32_t surfaceWidth  = 3'200U;
    constexpr std::uint32_t surfaceHeight = 2'000U;
    constexpr grab::geometry::Size benchSurface{
        .width  = surfaceWidth,
        .height = surfaceHeight,
    };

    constexpr grab::CoordinateSpaceId benchSpace{ 1U };
    constexpr std::size_t             defaultFrames    = 60U;
    constexpr std::size_t             warmupFrames     = 3U;
    constexpr std::chrono::milliseconds frameInterval{ 16 };
    constexpr std::chrono::milliseconds trailFade{ 1'200 };

    constexpr grab::overlay::Color benchColor{
        .r = 237U,
        .g = 206U,
        .b = 89U,
        .a = 230U,
    };
    constexpr float  trailStrokeWidth = 3.0F;
    constexpr float  shapeStrokeWidth = 4.0F;
    constexpr double millisecondsPerSecond = 1'000.0;
    constexpr double nanosecondsPerMillisecond = 1'000'000.0;
    constexpr double microsecondsPerMillisecond = 1'000.0;
    constexpr double percentile50 = 0.50;
    constexpr double percentile95 = 0.95;
    // A centre is half an extent; a margin applies at both ends.
    constexpr double halves       = 2.0;

    // Trail geometry. A pointer crossing the screen produces short segments
    // whose length is the distance travelled between two motion samples.
    constexpr double trailStepX      = 3.0;
    constexpr double trailStepY      = 1.75;
    constexpr double trailOriginX    = 40.0;
    constexpr double trailOriginY    = 40.0;
    constexpr double fastPointerScale = 8.0;

    // Large-shape geometry.
    constexpr double largeShapeX      = 120.0;
    constexpr double largeShapeY      = 100.0;
    constexpr double largeShapeWidth  = 2'400.0;
    constexpr double largeShapeHeight = 1'600.0;
    constexpr double growthPerFrame   = 2.0;

    // What batching would produce: the same 1200 samples carried by a bounded
    // number of polylines instead of one shape each.
    constexpr std::size_t batchedTrailShapes = 32U;
    constexpr std::size_t batchedTrailPoints = 38U;

    constexpr std::size_t smallTrailShapes  = 64U;
    constexpr std::size_t mediumTrailShapes = 150U;
    constexpr std::size_t largeTrailShapes  = 600U;
    constexpr std::size_t hugeTrailShapes   = 1'200U;

    // A pointer that reaches an edge turns round; it does not teleport to the
    // other side. Folding rather than wrapping matters here: a wrapped sample
    // makes one segment as wide as the screen, which is a property of the
    // synthetic path and not of anything grab does.
    [[nodiscard]]
    double
    reflected( double value,
               double range )
    {
        const auto period = range * halves;
        auto       folded = std::fmod( value, period );
        if( folded < 0.0 )
        {
            folded += period;
        }
        return folded <= range ? folded : period - folded;
    }

    [[nodiscard]]
    grab::SpacePoint
    point_at( double x,
              double y ) noexcept
    {
        return grab::SpacePoint{
            .x     = x,
            .y     = y,
            .space = benchSpace,
        };
    }

    // One two-point stroked path with a fade — byte for byte what
    // TrailAnimator::consume emits per motion sample.
    [[nodiscard]]
    grab::overlay::Shape
    trail_segment( grab::SpacePoint from,
                   grab::SpacePoint to )
    {
        std::vector<grab::overlay::PathCommand> commands;
        commands.reserve( 2U );
        commands.emplace_back( grab::overlay::MoveTo{ .point = from } );
        commands.emplace_back( grab::overlay::LineTo{ .point = to } );
        return grab::overlay::Shape{
            .geometry =
                grab::overlay::Path{
                                    .commands = std::move( commands ),
                                    .closed   = false,
                                    },
            .stroke =
                grab::overlay::StrokeStyle{
                                    .color    = benchColor,
                                    .width_px = trailStrokeWidth,
                                    },
            .fill     = std::nullopt,
            .lifetime = grab::overlay::Fade{ .duration = trailFade },
            .band     = grab::overlay::Band::Trail,
        };
    }

    // One stroked polyline: a batch of consecutive motion samples sharing a
    // single fade, which is what a batching TrailAnimator would emit.
    [[nodiscard]]
    grab::overlay::Shape
    trail_polyline( std::span<const grab::SpacePoint> points )
    {
        std::vector<grab::overlay::PathCommand> commands;
        commands.reserve( points.size() );
        commands.emplace_back( grab::overlay::MoveTo{ .point = points.front() } );
        for( const auto point : points.subspan( 1U ) )
        {
            commands.emplace_back( grab::overlay::LineTo{ .point = point } );
        }
        return grab::overlay::Shape{
            .geometry =
                grab::overlay::Path{
                                    .commands = std::move( commands ),
                                    .closed   = false,
                                    },
            .stroke =
                grab::overlay::StrokeStyle{
                                    .color    = benchColor,
                                    .width_px = trailStrokeWidth,
                                    },
            .fill     = std::nullopt,
            .lifetime = grab::overlay::Fade{ .duration = trailFade },
            .band     = grab::overlay::Band::Trail,
        };
    }

    [[nodiscard]]
    grab::overlay::Shape
    filled_rect( double x,
                 double y,
                 double width,
                 double height,
                 bool   stroked )
    {
        return grab::overlay::Shape{
            .geometry =
                grab::overlay::Rect{
                                    .bounds =
                        grab::SpaceRect{
                                    .x     = x,
                                    .y     = y,
                                    .w     = width,
                                    .h     = height,
                                    .space = benchSpace,
                                    },
                                    },
            .stroke = stroked ? std::optional<grab::overlay::StrokeStyle>{
                grab::overlay::StrokeStyle{
                                    .color    = benchColor,
                                    .width_px = shapeStrokeWidth,
                                    } } : std::nullopt,
            .fill =
                grab::overlay::FillStyle{
                                    .color = benchColor,
                                    },
            .lifetime = grab::overlay::Persistent{},
            .band     = grab::overlay::Band::Annotation,
        };
    }

    [[nodiscard]]
    grab::overlay::Shape
    big_ellipse( double radius_x,
                 double radius_y )
    {
        return grab::overlay::Shape{
            .geometry =
                grab::overlay::Ellipse{
                                       .center   = point_at( surfaceWidth / halves,
                                                             surfaceHeight / halves ),
                                       .radius_x = radius_x,
                                       .radius_y = radius_y,
                                       },
            .stroke   = std::nullopt,
            .fill     = grab::overlay::FillStyle{ .color = benchColor },
            .lifetime = grab::overlay::Persistent{},
            .band     = grab::overlay::Band::Annotation,
        };
    }

    struct Result
    {
            std::string name;
            std::size_t shapes         = 0;
            std::size_t damaged_pixels = 0;
            double      p50_ms         = 0.0;
            double      p95_ms         = 0.0;
            double      max_ms         = 0.0;
    };

    [[nodiscard]]
    double
    quantile( std::vector<double>& sorted,
              double               fraction )
    {
        if( sorted.empty() )
        {
            return 0.0;
        }
        const auto index = static_cast<std::size_t>(
            fraction * static_cast<double>( sorted.size() - 1U )
        );
        return sorted[index];
    }

    // Every scenario is the same loop: mutate the scene for frame `n`, then
    // time one render. The mutation is what makes damage realistic — a scene
    // that never changes damages nothing and measures nothing.
    template<typename Mutate>
    [[nodiscard]]
    Result
    run_scenario( std::string_view name,
                  OverlayRaster&   raster,
                  std::vector<grab::overlay::ShapeRecord>& scene,
                  std::size_t frames,
                  Mutate&&    mutate )
    {
        std::vector<double> samples;
        samples.reserve( frames );
        std::size_t damaged_pixels = 0;
        std::size_t shape_count    = 0;

        for( std::size_t frame = 0; frame < frames + warmupFrames; ++frame )
        {
            const auto now = frameInterval * static_cast<std::int64_t>( frame );
            mutate( scene, frame, now );
            std::ranges::sort( scene,
                               []( const grab::overlay::ShapeRecord& left,
                                   const grab::overlay::ShapeRecord& right )
                               {
                                   if( left.shape.band != right.shape.band )
                                   {
                                       return left.shape.band < right.shape.band;
                                   }
                                   if( left.shape.z != right.shape.z )
                                   {
                                       return left.shape.z < right.shape.z;
                                   }
                                   return left.id.slot < right.id.slot;
                               } );

            const auto started = Clock::now();
            auto       result  = raster.render( scene, now );
            const auto elapsed = Clock::now() - started;
            if( !result.has_value() )
            {
                std::fputs( "render failed\n", stderr );
                std::exit( EXIT_FAILURE );
            }
            if( frame < warmupFrames )
            {
                continue;
            }
            std::size_t frame_damage = 0;
            for( const auto rectangle : result->damage )
            {
                frame_damage += static_cast<std::size_t>( rectangle.width ) *
                                rectangle.height;
            }
            damaged_pixels = std::max( damaged_pixels, frame_damage );
            shape_count    = std::max( shape_count, scene.size() );
            samples.push_back(
                std::chrono::duration<double, std::milli>( elapsed ).count()
            );
        }

        std::ranges::sort( samples );
        return Result{
            .name           = std::string{ name },
            .shapes         = shape_count,
            .damaged_pixels = damaged_pixels,
            .p50_ms         = quantile( samples, percentile50 ),
            .p95_ms         = quantile( samples, percentile95 ),
            .max_ms         = samples.empty() ? 0.0 : samples.back(),
        };
    }

    // A fading trail of `count` segments with one fresh sample per frame. Every
    // segment's opacity changes every frame, so every segment is re-rasterized
    // every frame: this is the regime a fast pointer puts the overlay in.
    [[nodiscard]]
    Result
    trail_scenario( std::string_view name,
                    std::size_t      count,
                    double           step_scale,
                    std::size_t      frames )
    {
        auto raster = OverlayRaster::create( benchSurface );
        if( !raster.has_value() )
        {
            std::fputs( "raster creation failed\n", stderr );
            std::exit( EXIT_FAILURE );
        }
        std::vector<grab::overlay::ShapeRecord> scene;

        const auto sample_point = [step_scale]( std::size_t index )
        {
            const auto offset = static_cast<double>( index );
            const auto x =
                trailOriginX + reflected( offset * trailStepX * step_scale,
                                          surfaceWidth - ( trailOriginX * halves ) );
            const auto y =
                trailOriginY + reflected( offset * trailStepY * step_scale,
                                          surfaceHeight - ( trailOriginY * halves ) );
            return point_at( x, y );
        };

        return run_scenario(
            name,
            *raster,
            scene,
            frames,
            [count, &sample_point]( std::vector<grab::overlay::ShapeRecord>& shapes,
                                    std::size_t                              frame,
                                    std::chrono::milliseconds                now )
            {
                // Fill to `count` on the first frame, then retire the oldest
                // and append one fresh segment per frame.
                while( shapes.size() < count )
                {
                    const auto index = shapes.size() + frame;
                    shapes.push_back( grab::overlay::ShapeRecord{
                        .id =
                            grab::overlay::ShapeId{
                                                   .slot = static_cast<std::uint32_t>( index ),
                                                   },
                        .shape = trail_segment( sample_point( index ),
                                                sample_point( index + 1U ) ),
                        // Spread the fade phases so opacities differ, as they
                        // do in a live trail.
                        .started_at =
                            now - ( trailFade * static_cast<std::int64_t>(
                                                    shapes.size() % count
                                                ) ) /
                                      static_cast<std::int64_t>( count ),
                    } );
                }
                if( frame == 0 )
                {
                    return;
                }
                shapes.erase( shapes.begin() );
                const auto index = frame + count;
                shapes.push_back( grab::overlay::ShapeRecord{
                    .id =
                        grab::overlay::ShapeId{
                                               .slot = static_cast<std::uint32_t>( index ),
                                               },
                    .shape      = trail_segment( sample_point( index ),
                                                 sample_point( index + 1U ) ),
                    .started_at = now,
                } );
            }
        );
    }

    [[nodiscard]]
    Result
    batched_trail_scenario( std::string_view name,
                            std::size_t      frames )
    {
        auto raster = OverlayRaster::create( benchSurface );
        if( !raster.has_value() )
        {
            std::fputs( "raster creation failed\n", stderr );
            std::exit( EXIT_FAILURE );
        }
        std::vector<grab::overlay::ShapeRecord> scene;

        const auto sample_point = []( std::size_t index )
        {
            const auto offset = static_cast<double>( index );
            const auto x =
                trailOriginX +
                reflected( offset * trailStepX, surfaceWidth - ( trailOriginX * halves ) );
            const auto y =
                trailOriginY +
                reflected( offset * trailStepY,
                           surfaceHeight - ( trailOriginY * halves ) );
            return point_at( x, y );
        };

        return run_scenario(
            name,
            *raster,
            scene,
            frames,
            [&sample_point]( std::vector<grab::overlay::ShapeRecord>& shapes,
                             std::size_t                              frame,
                             std::chrono::milliseconds                now )
            {
                shapes.clear();
                for( std::size_t batch{}; batch < batchedTrailShapes; ++batch )
                {
                    std::vector<grab::SpacePoint> points;
                    points.reserve( batchedTrailPoints );
                    const auto first = frame + ( batch * batchedTrailPoints );
                    for( std::size_t step{}; step < batchedTrailPoints; ++step )
                    {
                        points.push_back( sample_point( first + step ) );
                    }
                    shapes.push_back( grab::overlay::ShapeRecord{
                        .id =
                            grab::overlay::ShapeId{
                                                   .slot = static_cast<std::uint32_t>( batch ),
                                                   },
                        .shape = trail_polyline( points ),
                        .started_at =
                            now - ( ( trailFade *
                                      static_cast<std::int64_t>( batch ) ) /
                                    static_cast<std::int64_t>( batchedTrailShapes ) ),
                    } );
                }
            }
        );
    }

    [[nodiscard]]
    Result
    large_shape_scenario( std::string_view name,
                          std::size_t      frames,
                          bool             stroked )
    {
        auto raster = OverlayRaster::create( benchSurface );
        if( !raster.has_value() )
        {
            std::fputs( "raster creation failed\n", stderr );
            std::exit( EXIT_FAILURE );
        }
        std::vector<grab::overlay::ShapeRecord> scene;
        return run_scenario(
            name,
            *raster,
            scene,
            frames,
            [stroked]( std::vector<grab::overlay::ShapeRecord>& shapes,
                       std::size_t                              frame,
                       std::chrono::milliseconds /*now*/ )
            {
                // A sketch drag: one shape whose extent grows every frame, so
                // damage is the union of the old and new bounds.
                const auto growth = static_cast<double>( frame ) * growthPerFrame;
                shapes.assign( 1U,
                               grab::overlay::ShapeRecord{
                                   .id    = grab::overlay::ShapeId{ .slot = 1U },
                                   .shape = filled_rect( largeShapeX,
                                                         largeShapeY,
                                                         largeShapeWidth + growth,
                                                         largeShapeHeight + growth,
                                                         stroked ),
                               } );
            }
        );
    }

    [[nodiscard]]
    Result
    large_ellipse_scenario( std::string_view name,
                            std::size_t      frames )
    {
        auto raster = OverlayRaster::create( benchSurface );
        if( !raster.has_value() )
        {
            std::fputs( "raster creation failed\n", stderr );
            std::exit( EXIT_FAILURE );
        }
        std::vector<grab::overlay::ShapeRecord> scene;
        return run_scenario(
            name,
            *raster,
            scene,
            frames,
            []( std::vector<grab::overlay::ShapeRecord>& shapes,
                std::size_t                              frame,
                std::chrono::milliseconds /*now*/ )
            {
                const auto growth = static_cast<double>( frame ) * growthPerFrame;
                shapes.assign( 1U,
                               grab::overlay::ShapeRecord{
                                   .id    = grab::overlay::ShapeId{ .slot = 1U },
                                   .shape = big_ellipse( largeShapeWidth / 2.0 + growth,
                                                         largeShapeHeight / 2.0 +
                                                             growth ),
                               } );
            }
        );
    }

    void
    print_row( const Result& result )
    {
        const auto per_shape_us = result.shapes == 0
                                      ? 0.0
                                      : ( result.p50_ms * microsecondsPerMillisecond ) /
                                            static_cast<double>( result.shapes );
        const auto per_pixel_ns =
            result.damaged_pixels == 0
                ? 0.0
                : ( result.p50_ms * nanosecondsPerMillisecond ) /
                      static_cast<double>( result.damaged_pixels );
        const auto fps = result.p50_ms <= 0.0 ? 0.0 : millisecondsPerSecond / result.p50_ms;
        std::printf( "%-22s %7zu %11zu %9.2f %9.2f %9.2f %8.1f %9.1f %8.2f\n",
                     result.name.c_str(),
                     result.shapes,
                     result.damaged_pixels,
                     result.p50_ms,
                     result.p95_ms,
                     result.max_ms,
                     fps,
                     per_shape_us,
                     per_pixel_ns );
    }

}    // namespace

int
main( int    argc,
      char** argv )
{
    std::size_t frames = defaultFrames;
    std::string only;
    const std::span<char*> args( argv, static_cast<std::size_t>( argc ) );
    for( std::size_t index = 1; index < args.size(); ++index )
    {
        const std::string_view argument{ args[index] };
        if( argument == "--frames" && index + 1U < args.size() )
        {
            frames = static_cast<std::size_t>( std::atoll( args[index + 1U] ) );
            ++index;
        }
        else if( argument == "--scenario" && index + 1U < args.size() )
        {
            only = args[index + 1U];
            ++index;
        }
    }

    std::printf( "surface %ux%u, %zu frames per scenario\n",
                 surfaceWidth,
                 surfaceHeight,
                 frames );
    std::printf( "%-22s %7s %11s %9s %9s %9s %8s %9s %8s\n",
                 "scenario",
                 "shapes",
                 "damaged_px",
                 "p50_ms",
                 "p95_ms",
                 "max_ms",
                 "fps",
                 "us/shape",
                 "ns/px" );

    const auto wanted = [&only]( std::string_view name )
    {
        return only.empty() || only == name;
    };

    if( wanted( "trail-64" ) )
    {
        print_row( trail_scenario( "trail-64", smallTrailShapes, 1.0, frames ) );
    }
    if( wanted( "trail-150" ) )
    {
        print_row( trail_scenario( "trail-150", mediumTrailShapes, 1.0, frames ) );
    }
    if( wanted( "trail-600" ) )
    {
        print_row( trail_scenario( "trail-600", largeTrailShapes, 1.0, frames ) );
    }
    if( wanted( "trail-1200" ) )
    {
        print_row( trail_scenario( "trail-1200", hugeTrailShapes, 1.0, frames ) );
    }
    if( wanted( "trail-600-fast" ) )
    {
        print_row( trail_scenario( "trail-600-fast",
                                   largeTrailShapes,
                                   fastPointerScale,
                                   frames ) );
    }
    if( wanted( "trail-batched" ) )
    {
        print_row( batched_trail_scenario( "trail-batched", frames ) );
    }
    if( wanted( "big-rect-fill" ) )
    {
        print_row( large_shape_scenario( "big-rect-fill", frames, false ) );
    }
    if( wanted( "big-rect-stroked" ) )
    {
        print_row( large_shape_scenario( "big-rect-stroked", frames, true ) );
    }
    if( wanted( "big-ellipse" ) )
    {
        print_row( large_ellipse_scenario( "big-ellipse", frames ) );
    }

    return EXIT_SUCCESS;
}
