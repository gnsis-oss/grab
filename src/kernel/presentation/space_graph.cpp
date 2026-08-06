#include "grab/result.hpp"
#include "grab/space.hpp"
#include "kernel/graph/adjacency_graph.hpp"
#include "kernel/graph/graph_traversal.hpp"
#include "kernel/presentation/space_graph.hpp"
#include "kernel/support/log.hpp"
#include "kernel/support/log_tags.hpp"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <map>
#include <utility>

namespace grab::detail
{

    namespace
    {

        using TransformGraph =
            kernel::AdjacencyGraph<CoordinateSpaceId, TransformRecord>;
        using GenerationLookup = std::map<CoordinateSpaceId, std::uint32_t>;

        struct RouteState
        {
                Affine         transform{};
                TransformTrust trust{ TransformTrust::Exact };
                bool           stale{};
        };

        [[nodiscard]]
        Affine
        compose( const Affine& first,
                 const Affine& second )
        {
            return Affine{
                .xx = ( second.xx * first.xx ) + ( second.xy * first.yx ),
                .xy = ( second.xx * first.xy ) + ( second.xy * first.yy ),
                .tx = ( second.xx * first.tx ) + ( second.xy * first.ty ) + second.tx,
                .yx = ( second.yx * first.xx ) + ( second.yy * first.yx ),
                .yy = ( second.yx * first.xy ) + ( second.yy * first.yy ),
                .ty = ( second.yx * first.tx ) + ( second.yy * first.ty ) + second.ty,
            };
        }

        [[nodiscard]]
        TransformTrust
        weakest( TransformTrust first,
                 TransformTrust second )
        {
            return std::max( first, second );
        }

        class RouteVisitor final
        {
            public:

                RouteVisitor( const TransformGraph&   graph,
                              const GenerationLookup& generations,
                              CoordinateSpaceId       source ) :
                    graph_{ &graph },
                    generations_{ &generations }
                {
                    routes_.emplace( source, RouteState{} );
                }

                void
                visit_node( CoordinateSpaceId /* space */ )
                {
                }

                // Breadth-first order guarantees `source` already has a route by
                // the time its outgoing edges are offered, so each destination
                // is reached by the shortest chain of transforms and the first
                // route recorded for it is the one kept.
                void
                visit_edge( CoordinateSpaceId source,
                            CoordinateSpaceId destination )
                {
                    if( routes_.contains( destination ) )
                    {
                        return;
                    }

                    const auto* transform = graph_->edge_payload( source, destination );
                    if( transform == nullptr )
                    {
                        return;
                    }

                    const auto& prior = routes_.at( source );
                    routes_.emplace(
                        destination,
                        RouteState{
                            .transform = compose( prior.transform, transform->map ),
                            .trust     = weakest( prior.trust, transform->trust ),
                            .stale     = prior.stale ||
                                         is_stale( *transform, source, destination ),
                        }
                    );
                }

                [[nodiscard]]
                const RouteState*
                route_to( CoordinateSpaceId destination ) const
                {
                    const auto route = routes_.find( destination );
                    return route == routes_.end() ? nullptr : &route->second;
                }

            private:

                [[nodiscard]]
                bool
                is_stale( const TransformRecord& transform,
                          CoordinateSpaceId      source,
                          CoordinateSpaceId      destination ) const
                {
                    return transform.generation <
                           generations_->at( source ) ||
                           transform.generation < generations_->at( destination );
                }

                const TransformGraph*                   graph_;
                const GenerationLookup*                 generations_;
                std::map<CoordinateSpaceId, RouteState> routes_;
        };

    }    // namespace

    CoordinateSpaceId
    SpaceGraph::add_space( std::uint32_t generation )
    {
        const CoordinateSpaceId space{ .value = next_space_ };
        ++next_space_;

        ( void )graph_.add_node( space );
        generations_.emplace( space, generation );
        return space;
    }

    void
    SpaceGraph::add_transform( TransformRecord transform )
    {
        const auto source      = transform.source;
        const auto destination = transform.destination;
        if( !graph_.add_edge( source, destination, transform ) )
        {
            // Either endpoint may be a space nobody registered, which is a
            // caller mistake rather than a runtime condition: say so instead of
            // leaving a route silently missing later.
            log::nominal(
                [&]( log::Event& event )
                {
                    event.tag( log::tags::space )
                        .value( "rejected", "transform" )
                        .value( "source", source.value )
                        .value( "destination", destination.value );
                }
            );
        }
    }

    void
    SpaceGraph::bump_generation( CoordinateSpaceId space )
    {
        ++generations_.at( space );
    }

    Result<Affine>
    SpaceGraph::resolve_transform( CoordinateSpaceId source,
                                   CoordinateSpaceId destination ) const
    {
        auto route = find_route( source, destination );
        if( !route.has_value() )
        {
            return std::unexpected( std::move( route.error() ) );
        }
        if( route->stale )
        {
            return fail( ErrorCode::TopologyChanged,
                         "coordinate transform route is stale" );
        }
        return route->transform;
    }

    Result<SpacePoint>
    SpaceGraph::map( SpacePoint        point,
                     CoordinateSpaceId destination ) const
    {
        auto transform = resolve_transform( point.space, destination );
        if( !transform.has_value() )
        {
            return std::unexpected( std::move( transform.error() ) );
        }

        return SpacePoint{
            .x     = ( transform->xx * point.x ) +
                     ( transform->xy * point.y ) +
                     transform->tx,
            .y     = ( transform->yx * point.x ) +
                     ( transform->yy * point.y ) +
                     transform->ty,
            .space = destination,
        };
    }

    Result<TransformTrust>
    SpaceGraph::route_trust( CoordinateSpaceId source,
                             CoordinateSpaceId destination ) const
    {
        auto route = find_route( source, destination );
        if( !route.has_value() )
        {
            return std::unexpected( std::move( route.error() ) );
        }
        if( route->stale )
        {
            return fail( ErrorCode::TopologyChanged,
                         "coordinate transform route is stale" );
        }
        return route->trust;
    }

    Result<SpaceGraph::Route>
    SpaceGraph::find_route( CoordinateSpaceId source,
                            CoordinateSpaceId destination ) const
    {
        if( !graph_.contains_node( source ) || !graph_.contains_node( destination ) )
        {
            return fail( ErrorCode::RouteUnavailable,
                         "coordinate space is not in the transform graph" );
        }

        RouteVisitor visitor{ graph_, generations_, source };
        kernel::breadth_first_search( graph_, source, visitor );
        const auto* route = visitor.route_to( destination );
        if( route == nullptr )
        {
            return fail( ErrorCode::RouteUnavailable,
                         "no coordinate transform route is available" );
        }

        return Route{
            .transform = route->transform,
            .trust     = route->trust,
            .stale     = route->stale,
        };
    }

}    // namespace grab::detail
