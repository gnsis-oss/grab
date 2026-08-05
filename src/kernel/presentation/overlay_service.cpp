#include "grab/capability.hpp"
#include "grab/overlay.hpp"
#include "grab/overlay_edit.hpp"
#include "grab/result.hpp"
#include "grab/space.hpp"
#include "kernel/presentation/overlay_animation.hpp"
#include "kernel/presentation/overlay_edit_session.hpp"
#include "kernel/presentation/overlay_scene.hpp"
#include "kernel/presentation/overlay_service.hpp"
#include "kernel/presentation/space_graph.hpp"
#include "spi/overlay_delegate.hpp"
#include "spi/runtime.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace grab::kernel::presentation
{
    namespace
    {

        constexpr std::size_t  rectangleCornerCount = 4U;
        constexpr std::size_t  cubicControlCount    = 3U;
        constexpr std::size_t  closingCommandCount  = 1U;
        constexpr std::size_t  firstPointOffset     = 1U;
        constexpr std::size_t  ellipseCommandCount  = 6U;
        constexpr double       ellipseControlFactor = 0.55228474983079339840;
        constexpr std::uint8_t primaryPointerButton = 1U;

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
        bool
        path_is_in_space( const overlay::Path& path,
                          CoordinateSpaceId    space ) noexcept
        {
            return std::ranges::all_of(
                path.commands,
                [space]( const overlay::PathCommand& command )
                {
                    if( const auto* move = std::get_if<overlay::MoveTo>( &command ) )
                    {
                        return move->point.space == space;
                    }
                    if( const auto* line = std::get_if<overlay::LineTo>( &command ) )
                    {
                        return line->point.space == space;
                    }
                    if( const auto* bezier = std::get_if<overlay::BezierTo>( &command ) )
                    {
                        return std::ranges::all_of( bezier->control,
                                                    [space]( SpacePoint point )
                                                    {
                                                        return point.space == space;
                                                    } );
                    }
                    return true;
                }
            );
        }

        [[nodiscard]]
        bool
        geometry_is_in_space( const overlay::Geometry& geometry,
                              CoordinateSpaceId        space ) noexcept
        {
            if( const auto* path = std::get_if<overlay::Path>( &geometry ) )
            {
                return path_is_in_space( *path, space );
            }
            if( const auto* rect = std::get_if<overlay::Rect>( &geometry ) )
            {
                return rect->bounds.space == space;
            }
            if( const auto* ellipse = std::get_if<overlay::Ellipse>( &geometry ) )
            {
                return ellipse->center.space == space;
            }
            const auto* polygon = std::get_if<overlay::Polygon>( &geometry );
            return polygon !=
                   nullptr &&
                   std::ranges::all_of( polygon->points,
                                        [space]( SpacePoint point )
                                        {
                                            return point.space == space;
                                        } );
        }

        [[nodiscard]]
        Result<overlay::Shape>
        transform_shape( const detail::SpaceGraph& graph,
                         CoordinateSpaceId         destination,
                         const overlay::Shape&     source )
        {
            if( source.animation.has_value() )
            {
                if( !valid_animation( *source.animation ) )
                {
                    return fail( ErrorCode::InvalidArgument,
                                 "overlay animation specification is invalid" );
                }
                if( !geometry_is_in_space( source.geometry, destination ) )
                {
                    return fail(
                        ErrorCode::InvalidArgument,
                        "animated overlay geometry must already be in delegate space"
                    );
                }
            }
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

        [[nodiscard]]
        Error
        callback_exception( const std::exception& exception )
        {
            return Error{
                .code    = ErrorCode::InternalFault,
                .message = std::string{ "overlay edit callback: " } + exception.what(),
                .capability = {},
                .target     = {},
                .attempts   = {},
            };
        }

        [[nodiscard]]
        Error
        unknown_callback_exception()
        {
            return Error{
                .code       = ErrorCode::InternalFault,
                .message    = "overlay edit callback failed",
                .capability = {},
                .target     = {},
                .attempts   = {},
            };
        }

        void
        stop_edit_session_noexcept(
            const std::shared_ptr<OverlayEditSession>& session
        ) noexcept
        {
            if( session == nullptr )
            {
                return;
            }
            try
            {
                [[maybe_unused]]
                auto stopped = session->stop();
            }
            catch( ... )
            {
                // A delegate exception cannot escape an error cleanup path.
                return;
            }
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
        std::optional<EditNotification>     notification;
        std::shared_ptr<OverlayEditSession> terminal_session;
        {
            const std::scoped_lock lock{ mutex_ };
            if( edit_session_ != nullptr )
            {
                terminal_session = edit_session_;
                try
                {
                    notification = cancel_drag_locked( terminal_session, true );
                    [[maybe_unused]]
                    auto stopped = terminate_edit_locked( terminal_session );
                }
                catch( ... )
                {
                    stop_edit_session_noexcept( terminal_session );
                }
            }
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
                }
                opened_ = false;
            }
            if( terminal_session != nullptr )
            {
                // close() is the terminal provider fallback (the X11 delegate
                // destroys its window and releases any surviving server grab).
                // Detach even when checked cleanup or close reported failure so
                // a public EditSession may safely outlive this service/runtime.
                terminal_session->detach_delegate();
            }
            edit_session_.reset();
        }
        invoke_notification( std::move( notification ) );
    }

    Result<overlay::ShapeId>
    OverlayService::add( overlay::Shape shape )
    {
        Result<overlay::ShapeId>        result;
        std::optional<EditNotification> notification;
        {
            const std::scoped_lock lock{ mutex_ };
            recover_best_effort();
            auto preflight = transform_shape( *graph_, delegate_space_, shape );
            if( !preflight.has_value() )
            {
                result = std::unexpected( std::move( preflight.error() ) );
            }
            else
            {
                result = scene_.add( std::move( shape ) );
            }
            notification = refresh_edit_locked();
        }
        invoke_notification( std::move( notification ) );
        return result;
    }

    Result<std::vector<overlay::ShapeId>>
    OverlayService::add_many( std::span<overlay::Shape> shapes )
    {
        Result<std::vector<overlay::ShapeId>> result;
        std::optional<EditNotification>       notification;
        {
            const std::scoped_lock lock{ mutex_ };
            recover_best_effort();

            std::optional<Error> preflight_error;
            // Preflight EVERY shape before adding ANY. A batch that fails halfway
            // would leave the scene holding a prefix the caller never asked to
            // stand alone, and it has no ids for what did land.
            for( overlay::Shape& shape : shapes )
            {
                auto preflight = transform_shape( *graph_, delegate_space_, shape );
                if( !preflight.has_value() )
                {
                    preflight_error = std::move( preflight.error() );
                    break;
                }
            }

            if( preflight_error.has_value() )
            {
                result = std::unexpected( std::move( *preflight_error ) );
            }
            else
            {
                std::vector<overlay::ShapeId> ids;
                ids.reserve( shapes.size() );
                for( overlay::Shape& shape : shapes )
                {
                    auto added = scene_.add( std::move( shape ) );
                    if( !added.has_value() )
                    {
                        result = std::unexpected( std::move( added.error() ) );
                        break;
                    }
                    ids.push_back( *added );
                }
                if( ids.size() == shapes.size() )
                {
                    result = std::move( ids );
                }
            }
            notification = refresh_edit_locked();
        }
        invoke_notification( std::move( notification ) );
        return result;
    }

    Result<void>
    OverlayService::update( overlay::ShapeId id,
                            overlay::Shape   shape )
    {
        Result<void>                    result;
        std::optional<EditNotification> notification;
        {
            const std::scoped_lock lock{ mutex_ };
            recover_best_effort();
            auto preflight = transform_shape( *graph_, delegate_space_, shape );
            if( !preflight.has_value() )
            {
                result = std::unexpected( std::move( preflight.error() ) );
            }
            else
            {
                result = scene_.update( id, std::move( shape ) );
                if( result.has_value() &&
                    edit_session_ !=
                    nullptr &&
                    edit_session_->dragging() &&
                    edit_session_->target() == id )
                {
                    notification = cancel_drag_locked( edit_session_, false );
                }
            }
            auto refreshed = refresh_edit_locked();
            if( !notification.has_value() )
            {
                notification = std::move( refreshed );
            }
        }
        invoke_notification( std::move( notification ) );
        return result;
    }

    Result<void>
    OverlayService::remove( overlay::ShapeId id )
    {
        Result<void>                    result;
        std::optional<EditNotification> notification;
        {
            const std::scoped_lock lock{ mutex_ };
            recover_best_effort();
            result = scene_.remove( id );
            if( result.has_value() &&
                edit_session_ !=
                nullptr &&
                edit_session_->dragging() &&
                edit_session_->target() == id )
            {
                notification = cancel_drag_locked( edit_session_, false );
            }
            auto refreshed = refresh_edit_locked();
            if( !notification.has_value() )
            {
                notification = std::move( refreshed );
            }
        }
        invoke_notification( std::move( notification ) );
        return result;
    }

    void
    OverlayService::clear()
    {
        std::optional<EditNotification> notification;
        {
            const std::scoped_lock lock{ mutex_ };
            recover_best_effort();
            scene_.clear();
            if( edit_session_ != nullptr && edit_session_->dragging() )
            {
                notification = cancel_drag_locked( edit_session_, false );
            }
            auto refreshed = refresh_edit_locked();
            if( !notification.has_value() )
            {
                notification = std::move( refreshed );
            }
        }
        invoke_notification( std::move( notification ) );
    }

    Result<void>
    OverlayService::flush()
    {
        Result<void>                    result;
        std::optional<EditNotification> notification;
        {
            const std::scoped_lock lock{ mutex_ };
            notification        = refresh_edit_locked();
            const auto snapshot = scene_.snapshot();
            if( desynchronized_ )
            {
                result = recover( snapshot );
            }
            if( result.has_value() )
            {
                result = delegate_->flush( snapshot.through_revision );
                if( !result.has_value() )
                {
                    // A failed fence may leave the delegate desynchronized
                    // (topology change, compositor churn). Recover and retry the
                    // fence ONCE — flush is idempotent — so one call heals.
                    desynchronized_ = true;
                    auto recovered  = recover( snapshot );
                    if( recovered.has_value() )
                    {
                        result = delegate_->flush( snapshot.through_revision );
                        if( !result.has_value() )
                        {
                            desynchronized_ = true;
                        }
                    }
                    else
                    {
                        result = std::unexpected( std::move( recovered.error() ) );
                    }
                }
            }
        }
        invoke_notification( std::move( notification ) );
        return result;
    }

    Result<std::shared_ptr<OverlayEditSession>>
    OverlayService::start_edit( std::span<const overlay::ShapeId> editable,
                                EditCallbacks                     callbacks )
    {
        const std::scoped_lock lock{ mutex_ };
        if( edit_session_ != nullptr && edit_session_->live() )
        {
            return fail( ErrorCode::SessionExists,
                         "an overlay edit session is already active" );
        }
        if( edit_session_ != nullptr )
        {
            auto pending = edit_session_;
            auto cleaned = terminate_edit_locked( pending );
            if( !cleaned.has_value() )
            {
                return std::unexpected( std::move( cleaned.error() ) );
            }
        }

        recover_best_effort();
        const auto snapshot = scene_.snapshot();
        for( const auto id : editable )
        {
            const auto record =
                std::ranges::find( snapshot.shapes, id, &overlay::ShapeRecord::id );
            if( record == snapshot.shapes.end() )
            {
                return fail( ErrorCode::StaleShape, "overlay edit shape id is stale" );
            }
            if( record->shape.animation.has_value() )
            {
                return fail( ErrorCode::InvalidArgument,
                             "animated overlay shapes are not editable" );
            }
            if( !geometry_is_in_space( record->shape.geometry, delegate_space_ ) )
            {
                return fail( ErrorCode::InvalidArgument,
                             "overlay edit geometry must already be in delegate space" );
            }
        }

        auto session = OverlayEditSession::start(
            *delegate_,
            delegate_space_,
            snapshot.shapes,
            std::vector<overlay::ShapeId>{ editable.begin(), editable.end() },
            std::move( callbacks ),
            [this]( std::shared_ptr<OverlayEditSession> active,
                    const spi::OverlayEditEvent&        event )
            {
                handle_edit_event( std::move( active ), event );
            }
        );
        if( !session.has_value() )
        {
            return std::unexpected( std::move( session.error() ) );
        }
        edit_session_ = *session;
        return *session;
    }

    Result<void>
    OverlayService::stop_edit( const std::shared_ptr<OverlayEditSession>& session )
    {
        if( session == nullptr )
        {
            return {};
        }

        Result<void>                    result;
        std::optional<EditNotification> notification;
        {
            const std::scoped_lock lock{ mutex_ };
            if( edit_session_ != session )
            {
                return {};
            }
            notification = cancel_drag_locked( session, true );
            if( edit_session_ == session )
            {
                result = terminate_edit_locked( session );
            }
        }
        invoke_notification( std::move( notification ) );
        return result;
    }

    void
    OverlayService::handle_edit_event( std::shared_ptr<OverlayEditSession> session,
                                       const spi::OverlayEditEvent& event ) noexcept
    {
        std::optional<EditNotification> notification;
        try
        {
            {
                const std::scoped_lock lock{ mutex_ };
                if( session == nullptr || edit_session_ != session || !session->live() )
                {
                    return;
                }

                notification = refresh_edit_locked();
                if( edit_session_ != session || !session->live() )
                {
                    // A refresh failure terminates the session transactionally.
                }
                else if( event.kind == spi::OverlayEditEventKind::ButtonPress )
                {
                    if( !notification.has_value() &&
                        !session->dragging() &&
                        event.button == primaryPointerButton )
                    {
                        const auto snapshot = scene_.snapshot();
                        if( session->begin( snapshot.shapes,
                                            event.position,
                                            event.button ) )
                        {
                            auto grabbed = session->grab_pointer();
                            if( !grabbed.has_value() )
                            {
                                auto error = std::move( grabbed.error() );
                                session->remember_error( error );
                                notification = cancel_drag_locked( session, false );
                                [[maybe_unused]]
                                auto stopped = terminate_edit_locked( session );
                            }
                        }
                    }
                }
                else if( event.kind == spi::OverlayEditEventKind::PointerMotion )
                {
                    if( !notification.has_value() && session->dragging() )
                    {
                        const auto target  = session->target();
                        auto       preview = session->update( event.position );
                        if( preview.has_value() )
                        {
                            auto applied = scene_.update( target, *preview );
                            if( !applied.has_value() )
                            {
                                const bool restore =
                                    applied.error().code != ErrorCode::StaleShape;
                                auto error   = std::move( applied.error() );
                                notification = cancel_drag_locked( session, restore );
                                if( error.code != ErrorCode::StaleShape )
                                {
                                    session->remember_error( error );
                                    [[maybe_unused]]
                                    auto stopped = terminate_edit_locked( session );
                                }
                            }
                            else
                            {
                                notification = refresh_edit_locked();
                            }
                        }
                    }
                }
                else if( event.kind == spi::OverlayEditEventKind::ButtonRelease )
                {
                    if( !notification.has_value() &&
                        session->dragging() &&
                        event.button == session->button() )
                    {
                        const auto target = session->target();
                        auto       final  = session->update( event.position );
                        if( final.has_value() )
                        {
                            auto applied = scene_.update( target, *final );
                            if( !applied.has_value() )
                            {
                                const bool restore =
                                    applied.error().code != ErrorCode::StaleShape;
                                auto error   = std::move( applied.error() );
                                notification = cancel_drag_locked( session, restore );
                                if( error.code != ErrorCode::StaleShape )
                                {
                                    session->remember_error( error );
                                    [[maybe_unused]]
                                    auto stopped = terminate_edit_locked( session );
                                }
                            }
                            else
                            {
                                notification = refresh_edit_locked();
                                if( edit_session_ == session && session->dragging() )
                                {
                                    auto released = session->release_pointer();
                                    if( !released.has_value() )
                                    {
                                        notification =
                                            cancel_drag_locked( session, true );
                                        [[maybe_unused]]
                                        auto stopped = terminate_edit_locked( session );
                                    }
                                    else
                                    {
                                        auto committed =
                                            session->commit( event.position );
                                        if( committed.has_value() )
                                        {
                                            session->finish_drag();
                                            notification = EditNotification{
                                                .session = session,
                                                .id      = target,
                                                .shape   = std::move( *committed ),
                                            };
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                else if( event.kind == spi::OverlayEditEventKind::NotifyUngrab )
                {
                    session->pointer_was_ungrabbed();
                    if( session->dragging() )
                    {
                        notification   = cancel_drag_locked( session, true );
                        auto refreshed = refresh_edit_locked();
                        if( !notification.has_value() )
                        {
                            notification = std::move( refreshed );
                        }
                    }
                }
            }
            invoke_notification( std::move( notification ) );
        }
        catch( const std::exception& exception )
        {
            try
            {
                abort_edit_after_exception( session, callback_exception( exception ) );
            }
            catch( ... )
            {
                stop_edit_session_noexcept( session );
            }
        }
        catch( ... )
        {
            try
            {
                abort_edit_after_exception( session, unknown_callback_exception() );
            }
            catch( ... )
            {
                stop_edit_session_noexcept( session );
            }
        }
    }

    void
    OverlayService::abort_edit_after_exception(
        std::shared_ptr<OverlayEditSession> session,
        Error                               error
    ) noexcept
    {
        if( session == nullptr )
        {
            return;
        }
        session->remember_error( std::move( error ) );

        std::optional<EditNotification> notification;
        try
        {
            {
                const std::scoped_lock lock{ mutex_ };
                if( edit_session_ == session )
                {
                    notification = cancel_drag_locked( session, true );
                    [[maybe_unused]]
                    auto stopped = terminate_edit_locked( session );
                }
            }
            invoke_notification( std::move( notification ) );
        }
        catch( ... )
        {
            stop_edit_session_noexcept( session );
        }
    }

    std::optional<OverlayService::EditNotification>
    OverlayService::cancel_drag_locked( std::shared_ptr<OverlayEditSession> session,
                                        bool restore_original )
    {
        if( session == nullptr || !session->dragging() )
        {
            return std::nullopt;
        }

        const auto id = session->target();
        if( restore_original && session->original_shape().has_value() )
        {
            auto restored = scene_.update( id, *session->original_shape() );
            if( !restored.has_value() && restored.error().code != ErrorCode::StaleShape )
            {
                session->remember_error( std::move( restored.error() ) );
            }
        }
        session->cancel_interaction();
        auto released = session->release_pointer();
        if( !released.has_value() )
        {
            [[maybe_unused]]
            auto stopped = terminate_edit_locked( session );
        }
        return EditNotification{
            .session = session,
            .id      = id,
            .shape   = std::nullopt,
        };
    }

    std::optional<OverlayService::EditNotification>
    OverlayService::refresh_edit_locked()
    {
        auto session = edit_session_;
        if( session == nullptr )
        {
            return std::nullopt;
        }
        if( !session->live() )
        {
            [[maybe_unused]]
            auto stopped = terminate_edit_locked( session );
            return std::nullopt;
        }

        auto                            snapshot = scene_.snapshot();
        std::optional<EditNotification> notification;
        if( session->dragging() )
        {
            const auto record = std::ranges::find( snapshot.shapes,
                                                   session->target(),
                                                   &overlay::ShapeRecord::id );
            if( record == snapshot.shapes.end() )
            {
                notification = cancel_drag_locked( session, false );
                snapshot     = scene_.snapshot();
            }
        }
        if( edit_session_ != session )
        {
            return notification;
        }

        const auto editable          = session->editable();
        const auto became_ineligible = std::ranges::find_if(
            snapshot.shapes,
            [this, editable]( const overlay::ShapeRecord& record )
            {
                const bool selected =
                    std::ranges::find( editable, record.id ) != editable.end();
                return selected && ( record.shape.animation.has_value() ||
                                     !geometry_is_in_space( record.shape.geometry,
                                                            delegate_space_ ) );
            }
        );
        if( became_ineligible != snapshot.shapes.end() )
        {
            if( !notification.has_value() && session->dragging() )
            {
                notification = cancel_drag_locked( session, true );
            }
            session->remember_error( Error{
                .code       = ErrorCode::InvalidArgument,
                .message    = "an editable overlay shape became ineligible",
                .capability = {},
                .target     = {},
                .attempts   = {},
            } );
            [[maybe_unused]]
            auto stopped = terminate_edit_locked( session );
            return notification;
        }

        auto refreshed = session->refresh_region( snapshot.shapes );
        if( !refreshed.has_value() )
        {
            auto error = std::move( refreshed.error() );
            if( !notification.has_value() && session->dragging() )
            {
                notification = cancel_drag_locked( session, true );
            }
            session->remember_error( error );
            [[maybe_unused]]
            auto stopped = terminate_edit_locked( session );
        }
        return notification;
    }

    Result<void>
    OverlayService::terminate_edit_locked(
        const std::shared_ptr<OverlayEditSession>& session
    )
    {
        if( session == nullptr )
        {
            return {};
        }

        auto stopped = session->stop();
        if( !stopped.has_value() )
        {
            // A checked X request or fake failure may be transient.  Complete
            // RAII teardown in this same reactor turn when one retry suffices;
            // remember_error() still preserves the first failure for status().
            auto retried = session->stop();
            if( !retried.has_value() )
            {
                // Keep service ownership while delegate cleanup is incomplete.
                // A later verb/start can retry on the reactor thread; resetting
                // here would either leak X state or make the public session's
                // eventual destruction dereference a dead runtime.
                return std::unexpected( std::move( retried.error() ) );
            }
        }

        session->detach_delegate();
        if( edit_session_ == session )
        {
            edit_session_.reset();
        }
        return {};
    }

    void
    OverlayService::invoke_notification(
        std::optional<EditNotification> notification
    ) noexcept
    {
        if( !notification.has_value() || notification->session == nullptr )
        {
            return;
        }
        try
        {
            if( notification->shape.has_value() )
            {
                auto callback = notification->session->on_edit();
                if( callback )
                {
                    callback( notification->id, *notification->shape );
                }
            }
            else
            {
                auto callback = notification->session->on_cancelled();
                if( callback )
                {
                    callback( notification->id );
                }
            }
        }
        catch( const std::exception& exception )
        {
            notification->session->remember_error( callback_exception( exception ) );
        }
        catch( ... )
        {
            notification->session->remember_error( unknown_callback_exception() );
        }
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
