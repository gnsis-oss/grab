#include "grab/capability.hpp"
#include "grab/overlay.hpp"
#include "grab/result.hpp"
#include "grab/space.hpp"
#include "kernel/presentation/overlay_scene.hpp"
#include "kernel/presentation/overlay_service.hpp"
#include "kernel/presentation/space_graph.hpp"
#include "spi/overlay_delegate.hpp"
#include "spi/runtime.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <expected>
#include <iterator>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace grab::kernel::presentation
{
    namespace
    {

        constexpr std::size_t rectangleCornerCount = 4U;
        constexpr std::size_t cubicControlCount    = 3U;
        constexpr std::size_t closingCommandCount  = 1U;
        constexpr std::size_t firstPointOffset     = 1U;
        constexpr std::size_t ellipseCommandCount  = 6U;
        constexpr double      ellipseControlFactor = 0.55228474983079339840;

        [[nodiscard]]
        bool
        is_axis_scale_translate( const Affine& transform ) noexcept
        {
            return transform.xy == 0.0 && transform.yx == 0.0;
        }

        [[nodiscard]]
        SpacePoint
        apply( const Affine&     transform,
               SpacePoint        point,
               CoordinateSpaceId destination ) noexcept
        {
            return SpacePoint{
                .x     = ( transform.xx * point.x ) +
                         ( transform.xy * point.y ) +
                         transform.tx,
                .y     = ( transform.yx * point.x ) +
                         ( transform.yy * point.y ) +
                         transform.ty,
                .space = destination,
            };
        }

        [[nodiscard]]
        Result<Affine>
        resolve( const detail::SpaceGraph& graph,
                 CoordinateSpaceId         source,
                 CoordinateSpaceId         destination )
        {
            if( source == destination )
            {
                return Affine{};
            }
            return graph.resolve_transform( source, destination );
        }

        [[nodiscard]]
        Result<SpacePoint>
        transform_point( const detail::SpaceGraph& graph,
                         CoordinateSpaceId         destination,
                         SpacePoint                point )
        {
            auto transform = resolve( graph, point.space, destination );
            if( !transform.has_value() )
            {
                return std::unexpected( std::move( transform.error() ) );
            }
            return apply( *transform, point, destination );
        }

        [[nodiscard]]
        overlay::Path
        closed_path( std::span<const SpacePoint> points )
        {
            overlay::Path path;
            path.closed = true;
            if( points.empty() )
            {
                return path;
            }

            path.commands.reserve( points.size() + closingCommandCount );
            path.commands.emplace_back( overlay::MoveTo{ .point = points.front() } );
            for( const auto point : points.subspan( firstPointOffset ) )
            {
                path.commands.emplace_back( overlay::LineTo{ .point = point } );
            }
            path.commands.emplace_back( overlay::ClosePath{} );
            return path;
        }

        [[nodiscard]]
        Result<overlay::Path>
        transform_path( const detail::SpaceGraph& graph,
                        CoordinateSpaceId         destination,
                        const overlay::Path&      source )
        {
            overlay::Path result;
            result.closed = source.closed;
            result.commands.reserve( source.commands.size() );
            for( const auto& command : source.commands )
            {
                if( const auto* move = std::get_if<overlay::MoveTo>( &command ) )
                {
                    auto point = transform_point( graph, destination, move->point );
                    if( !point.has_value() )
                    {
                        return std::unexpected( std::move( point.error() ) );
                    }
                    result.commands.emplace_back( overlay::MoveTo{ .point = *point } );
                    continue;
                }
                if( const auto* line = std::get_if<overlay::LineTo>( &command ) )
                {
                    auto point = transform_point( graph, destination, line->point );
                    if( !point.has_value() )
                    {
                        return std::unexpected( std::move( point.error() ) );
                    }
                    result.commands.emplace_back( overlay::LineTo{ .point = *point } );
                    continue;
                }
                if( const auto* bezier = std::get_if<overlay::BezierTo>( &command ) )
                {
                    overlay::BezierTo transformed;
                    transformed.control.reserve( bezier->control.size() );
                    for( const auto control : bezier->control )
                    {
                        auto point = transform_point( graph, destination, control );
                        if( !point.has_value() )
                        {
                            return std::unexpected( std::move( point.error() ) );
                        }
                        transformed.control.push_back( *point );
                    }
                    result.commands.emplace_back( std::move( transformed ) );
                    continue;
                }
                result.commands.emplace_back( overlay::ClosePath{} );
            }
            return result;
        }

        [[nodiscard]]
        Result<overlay::Geometry>
        transform_rect( const detail::SpaceGraph& graph,
                        CoordinateSpaceId         destination,
                        const overlay::Rect&      source )
        {
            if( source.bounds.space == destination )
            {
                return overlay::Geometry{ source };
            }

            auto transform = resolve( graph, source.bounds.space, destination );
            if( !transform.has_value() )
            {
                return std::unexpected( std::move( transform.error() ) );
            }

            const auto left   = source.bounds.x;
            const auto top    = source.bounds.y;
            const auto right  = left + source.bounds.w;
            const auto bottom = top + source.bounds.h;
            if( is_axis_scale_translate( *transform ) )
            {
                const auto first = apply(
                    *transform,
                    SpacePoint{ .x = left, .y = top, .space = source.bounds.space },
                    destination
                );
                const auto opposite = apply(
                    *transform,
                    SpacePoint{ .x = right, .y = bottom, .space = source.bounds.space },
                    destination
                );
                const auto result_left   = std::min( first.x, opposite.x );
                const auto result_top    = std::min( first.y, opposite.y );
                const auto result_right  = std::max( first.x, opposite.x );
                const auto result_bottom = std::max( first.y, opposite.y );
                return overlay::Geometry{
                    overlay::Rect{
                                  .bounds = {
                            .x     = result_left,
                            .y     = result_top,
                            .w     = result_right - result_left,
                            .h     = result_bottom - result_top,
                            .space = destination,
                        }, }
                };
            }

            const std::array<SpacePoint, rectangleCornerCount> corners{
                SpacePoint{ .x = left,    .y = top, .space = source.bounds.space},
                SpacePoint{.x = right,    .y = top, .space = source.bounds.space},
                SpacePoint{.x = right, .y = bottom, .space = source.bounds.space},
                SpacePoint{ .x = left, .y = bottom, .space = source.bounds.space},
            };
            std::array<SpacePoint, rectangleCornerCount> transformed{};
            std::ranges::transform( corners,
                                    transformed.begin(),
                                    [&transform, destination]( SpacePoint point )
                                    {
                                        return apply( *transform, point, destination );
                                    } );
            return overlay::Geometry{ closed_path( transformed ) };
        }

        [[nodiscard]]
        Result<overlay::Geometry>
        transform_ellipse( const detail::SpaceGraph& graph,
                           CoordinateSpaceId         destination,
                           const overlay::Ellipse&   source )
        {
            if( source.center.space == destination )
            {
                return overlay::Geometry{ source };
            }

            auto transform = resolve( graph, source.center.space, destination );
            if( !transform.has_value() )
            {
                return std::unexpected( std::move( transform.error() ) );
            }
            if( is_axis_scale_translate( *transform ) )
            {
                return overlay::Geometry{
                    overlay::Ellipse{
                                     .center   = apply( *transform, source.center, destination ),
                                     .radius_x = std::abs( transform->xx ) * source.radius_x,
                                     .radius_y = std::abs( transform->yy ) * source.radius_y,
                                     }
                };
            }

            const auto center_x  = source.center.x;
            const auto center_y  = source.center.y;
            const auto radius_x  = source.radius_x;
            const auto radius_y  = source.radius_y;
            const auto control_x = ellipseControlFactor * radius_x;
            const auto control_y = ellipseControlFactor * radius_y;
            const auto point     = [&source]( double x, double y )
            {
                return SpacePoint{ .x = x, .y = y, .space = source.center.space };
            };
            const auto mapped = [&transform, destination]( SpacePoint value )
            {
                return apply( *transform, value, destination );
            };

            overlay::Path path;
            path.closed = true;
            path.commands.reserve( ellipseCommandCount );
            path.commands.emplace_back( overlay::MoveTo{
                .point = mapped( point( center_x + radius_x, center_y ) ),
            } );

            const auto append_cubic =
                [&path, &mapped]( std::array<SpacePoint, cubicControlCount> controls )
            {
                overlay::BezierTo segment;
                segment.control.reserve( controls.size() );
                std::ranges::transform( controls,
                                        std::back_inserter( segment.control ),
                                        mapped );
                path.commands.emplace_back( std::move( segment ) );
            };

            append_cubic( {
                point( center_x + radius_x, center_y + control_y ),
                point( center_x + control_x, center_y + radius_y ),
                point( center_x, center_y + radius_y ),
            } );
            append_cubic( {
                point( center_x - control_x, center_y + radius_y ),
                point( center_x - radius_x, center_y + control_y ),
                point( center_x - radius_x, center_y ),
            } );
            append_cubic( {
                point( center_x - radius_x, center_y - control_y ),
                point( center_x - control_x, center_y - radius_y ),
                point( center_x, center_y - radius_y ),
            } );
            append_cubic( {
                point( center_x + control_x, center_y - radius_y ),
                point( center_x + radius_x, center_y - control_y ),
                point( center_x + radius_x, center_y ),
            } );
            path.commands.emplace_back( overlay::ClosePath{} );
            return overlay::Geometry{ std::move( path ) };
        }

        [[nodiscard]]
        Result<overlay::Geometry>
        transform_polygon( const detail::SpaceGraph& graph,
                           CoordinateSpaceId         destination,
                           const overlay::Polygon&   source )
        {
            std::vector<SpacePoint> transformed;
            transformed.reserve( source.points.size() );
            bool lower_to_path{};
            for( const auto point : source.points )
            {
                auto transform = resolve( graph, point.space, destination );
                if( !transform.has_value() )
                {
                    return std::unexpected( std::move( transform.error() ) );
                }
                lower_to_path = lower_to_path || !is_axis_scale_translate( *transform );
                transformed.push_back( apply( *transform, point, destination ) );
            }
            if( lower_to_path )
            {
                return overlay::Geometry{ closed_path( transformed ) };
            }
            return overlay::Geometry{
                overlay::Polygon{
                                 .points = std::move( transformed ),
                                 }
            };
        }

        [[nodiscard]]
        Result<overlay::Geometry>
        transform_geometry( const detail::SpaceGraph& graph,
                            CoordinateSpaceId         destination,
                            const overlay::Geometry&  source )
        {
            if( const auto* path = std::get_if<overlay::Path>( &source ) )
            {
                auto transformed = transform_path( graph, destination, *path );
                if( !transformed.has_value() )
                {
                    return std::unexpected( std::move( transformed.error() ) );
                }
                return overlay::Geometry{ std::move( *transformed ) };
            }
            if( const auto* rect = std::get_if<overlay::Rect>( &source ) )
            {
                return transform_rect( graph, destination, *rect );
            }
            if( const auto* ellipse = std::get_if<overlay::Ellipse>( &source ) )
            {
                return transform_ellipse( graph, destination, *ellipse );
            }
            return transform_polygon( graph,
                                      destination,
                                      std::get<overlay::Polygon>( source ) );
        }

        [[nodiscard]]
        Result<overlay::Shape>
        transform_shape( const detail::SpaceGraph& graph,
                         CoordinateSpaceId         destination,
                         const overlay::Shape&     source )
        {
            auto geometry = transform_geometry( graph, destination, source.geometry );
            if( !geometry.has_value() )
            {
                return std::unexpected( std::move( geometry.error() ) );
            }
            auto result     = source;
            result.geometry = std::move( *geometry );
            return result;
        }

        [[nodiscard]]
        Result<overlay::SceneDelta>
        transform_delta( const detail::SpaceGraph&  graph,
                         CoordinateSpaceId          destination,
                         const overlay::SceneDelta& source )
        {
            auto  result = source;
            auto* upsert = std::get_if<overlay::Upsert>( &result.change );
            if( upsert == nullptr )
            {
                return result;
            }
            auto shape = transform_shape( graph, destination, upsert->record.shape );
            if( !shape.has_value() )
            {
                return std::unexpected( std::move( shape.error() ) );
            }
            upsert->record.shape = std::move( *shape );
            return result;
        }

        [[nodiscard]]
        Result<overlay::SceneSnapshot>
        transform_snapshot( const detail::SpaceGraph&     graph,
                            CoordinateSpaceId             destination,
                            const overlay::SceneSnapshot& source )
        {
            overlay::SceneSnapshot result{
                .epoch            = source.epoch,
                .through_revision = source.through_revision,
                .shapes           = {},
            };
            result.shapes.reserve( source.shapes.size() );
            for( const auto& record : source.shapes )
            {
                auto shape = transform_shape( graph, destination, record.shape );
                if( !shape.has_value() )
                {
                    return std::unexpected( std::move( shape.error() ) );
                }
                auto transformed_record  = record;
                transformed_record.shape = std::move( *shape );
                result.shapes.push_back( std::move( transformed_record ) );
            }
            return result;
        }

    }    // namespace

    OverlayService::OverlayService( spi::OverlayDelegate&     delegate,
                                    const detail::SpaceGraph& graph,
                                    CoordinateSpaceId         delegate_space,
                                    OverlayScene::Clock       clock ) :
        delegate_{ &delegate },
        graph_{ &graph },
        delegate_space_{ delegate_space },
        scene_{ std::move( clock ) }
    {
        scene_.set_delta_sink(
            [this]( const overlay::SceneDelta& delta )
            {
                publish( delta );
            }
        );
    }

    Result<std::unique_ptr<OverlayService>>
    OverlayService::create( spi::Runtime&             runtime,
                            const detail::SpaceGraph& graph,
                            CoordinateSpaceId         delegate_space,
                            OverlayScene::Clock       clock )
    {
        auto* const delegate = runtime.overlay_delegate();
        if( delegate == nullptr )
        {
            const std::string runtime_name{ runtime.name() };
            const std::string capability_name{ capability::overlay };
            return std::unexpected( Error{
                .code       = ErrorCode::CapabilityUnavailable,
                .message    = "runtime " +
                              runtime_name +
                              " has no overlay delegate for " +
                              capability_name,
                .capability = capability_name,
                .target     = runtime_name,
                .attempts   = {
                               ProviderAttempt{
                        .provider = runtime_name,
                        .reason   = "runtime has no overlay delegate",
                    }, },
            } );
        }

        std::unique_ptr<OverlayService> service{
            new OverlayService{ *delegate, graph, delegate_space, std::move( clock ) }
        };
        auto opened = delegate->open( delegate_space );
        if( !opened.has_value() )
        {
            return std::unexpected( std::move( opened.error() ) );
        }
        service->opened_ = true;
        return service;
    }

    OverlayService::~OverlayService()
    {
        const std::scoped_lock lock{ mutex_ };
        scene_.set_delta_sink( {} );
        if( opened_ )
        {
            try
            {
                delegate_->close();
            }
            catch( ... )
            {
                // Teardown cannot surface an error through this destructor.
                opened_ = false;
                return;
            }
            opened_ = false;
        }
    }

    Result<overlay::ShapeId>
    OverlayService::add( overlay::Shape shape )
    {
        const std::scoped_lock lock{ mutex_ };
        recover_best_effort();
        auto preflight = transform_shape( *graph_, delegate_space_, shape );
        if( !preflight.has_value() )
        {
            return std::unexpected( std::move( preflight.error() ) );
        }
        return scene_.add( std::move( shape ) );
    }

    Result<void>
    OverlayService::update( overlay::ShapeId id,
                            overlay::Shape   shape )
    {
        const std::scoped_lock lock{ mutex_ };
        recover_best_effort();
        auto preflight = transform_shape( *graph_, delegate_space_, shape );
        if( !preflight.has_value() )
        {
            return std::unexpected( std::move( preflight.error() ) );
        }
        return scene_.update( id, std::move( shape ) );
    }

    Result<void>
    OverlayService::remove( overlay::ShapeId id )
    {
        const std::scoped_lock lock{ mutex_ };
        recover_best_effort();
        return scene_.remove( id );
    }

    void
    OverlayService::clear()
    {
        const std::scoped_lock lock{ mutex_ };
        recover_best_effort();
        scene_.clear();
    }

    Result<void>
    OverlayService::flush()
    {
        const std::scoped_lock lock{ mutex_ };
        const auto             snapshot = scene_.snapshot();
        if( desynchronized_ )
        {
            auto recovered = recover( snapshot );
            if( !recovered.has_value() )
            {
                return std::unexpected( std::move( recovered.error() ) );
            }
        }
        auto flushed = delegate_->flush( snapshot.through_revision );
        if( !flushed.has_value() )
        {
            // A failed fence may leave the delegate desynchronized (topology
            // change, compositor churn). Recover and retry the fence ONCE —
            // flush is idempotent — so a single call heals instead of
            // returning ResyncRequired to the caller.
            desynchronized_ = true;
            auto recovered  = recover( snapshot );
            if( !recovered.has_value() )
            {
                return std::unexpected( std::move( recovered.error() ) );
            }
            flushed = delegate_->flush( snapshot.through_revision );
            if( !flushed.has_value() )
            {
                desynchronized_ = true;
            }
        }
        return flushed;
    }

    void
    OverlayService::publish( const overlay::SceneDelta& delta )
    {
        try
        {
            if( desynchronized_ )
            {
                return;
            }
            auto transformed = transform_delta( *graph_, delegate_space_, delta );
            if( !transformed.has_value() )
            {
                desynchronized_ = true;
                return;
            }
            const std::array deltas{ std::move( *transformed ) };
            auto             applied = delegate_->apply( deltas );
            if( !applied.has_value() )
            {
                desynchronized_ = true;
            }
        }
        catch( ... )
        {
            desynchronized_ = true;
        }
    }

    void
    OverlayService::recover_best_effort()
    {
        if( !desynchronized_ )
        {
            return;
        }
        const auto snapshot = scene_.snapshot();
        const auto recovery = recover( snapshot );
        if( !recovery.has_value() )
        {
            return;
        }
    }

    Result<void>
    OverlayService::recover( const overlay::SceneSnapshot& snapshot )
    {
        auto transformed = transform_snapshot( *graph_, delegate_space_, snapshot );
        if( !transformed.has_value() )
        {
            desynchronized_ = true;
            return std::unexpected( std::move( transformed.error() ) );
        }
        auto result = delegate_->resync( *transformed );
        if( !result.has_value() )
        {
            desynchronized_ = true;
            return std::unexpected( std::move( result.error() ) );
        }
        desynchronized_ = false;
        return {};
    }

}    // namespace grab::kernel::presentation
