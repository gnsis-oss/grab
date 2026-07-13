#include "core/space_graph.hpp"
#include "grab/result.hpp"
#include "grab/space.hpp"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <map>
#include <unordered_map>
#include <utility>
#include <walk/sweep.hpp>
#include <web/knot.hpp>
#include <web/web.hpp>

namespace grab::detail
{

    namespace
    {

        using TransformGraph   = web::Web<web::OneWay, TransformRecord>;
        using SpaceLookup      = std::map<web::Knot, CoordinateSpaceId>;
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
                              const SpaceLookup&      spaces,
                              const GenerationLookup& generations,
                              web::Knot               source ) :
                    graph_{ &graph },
                    spaces_{ &spaces },
                    generations_{ &generations }
                {
                    routes_.emplace( source, RouteState{} );
                }

                void
                on( web::Knot /* knot */ )
                {
                }

                void
                on_edge( web::Knot source,
                         web::Knot destination )
                {
                    if( routes_.contains( destination ) )
                    {
                        return;
                    }

                    const auto neighbors = graph_->out( source );
                    const auto edge =
                        std::ranges::find_if( neighbors,
                                              [destination]( const auto& candidate )
                                              {
                                                  return candidate.target == destination;
                                              } );
                    if( edge == neighbors.end() )
                    {
                        return;
                    }

                    const auto& prior = routes_.at( source );
                    routes_.emplace(
                        destination,
                        RouteState{
                            .transform = compose( prior.transform, edge->data.map ),
                            .trust     = weakest( prior.trust, edge->data.trust ),
                            .stale     = prior.stale ||
                                         is_stale( edge->data, source, destination ),
                        }
                    );
                }

                [[nodiscard]]
                const RouteState*
                route_to( web::Knot destination ) const
                {
                    const auto route = routes_.find( destination );
                    return route == routes_.end() ? nullptr : &route->second;
                }

            private:

                [[nodiscard]]
                bool
                is_stale( const TransformRecord& transform,
                          web::Knot              source,
                          web::Knot              destination ) const
                {
                    const auto source_space      = spaces_->at( source );
                    const auto destination_space = spaces_->at( destination );
                    return transform.generation <
                           generations_->at( source_space ) ||
                           transform.generation < generations_->at( destination_space );
                }

                const TransformGraph*                     graph_;
                const SpaceLookup*                        spaces_;
                const GenerationLookup*                   generations_;
                std::unordered_map<web::Knot, RouteState> routes_;
        };

    }    // namespace

    CoordinateSpaceId
    SpaceGraph::add_space()
    {
        const CoordinateSpaceId space{ .value = next_space_ };
        ++next_space_;
        const web::Knot knot{ static_cast<std::uint64_t>( space.value ) };

        ( void )graph_.add( knot );
        knots_.emplace( space, knot );
        spaces_.emplace( knot, space );
        generations_.emplace( space, 0U );
        return space;
    }

    void
    SpaceGraph::add_transform( TransformRecord transform )
    {
        const auto source      = knots_.at( transform.source );
        const auto destination = knots_.at( transform.destination );
        ( void )graph_.tie( source, destination, transform );
    }

    void
    SpaceGraph::bump_generation( CoordinateSpaceId space )
    {
        ++generations_.at( space );
    }

    Result<SpacePoint>
    SpaceGraph::map( SpacePoint        point,
                     CoordinateSpaceId destination ) const
    {
        auto route = find_route( point.space, destination );
        if( !route.has_value() )
        {
            return std::unexpected( std::move( route.error() ) );
        }
        if( route->stale )
        {
            return fail( ErrorCode::TopologyChanged,
                         "coordinate transform route is stale" );
        }

        const auto& transform = route->transform;
        return SpacePoint{
            .x = ( transform.xx * point.x ) + ( transform.xy * point.y ) + transform.tx,
            .y = ( transform.yx * point.x ) + ( transform.yy * point.y ) + transform.ty,
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
        const auto source_knot      = knots_.find( source );
        const auto destination_knot = knots_.find( destination );
        if( source_knot == knots_.end() || destination_knot == knots_.end() )
        {
            return fail( ErrorCode::RouteUnavailable,
                         "coordinate space is not in the transform graph" );
        }

        RouteVisitor visitor{ graph_, spaces_, generations_, source_knot->second };
        walk::sweep( graph_, source_knot->second, visitor );
        const auto* route = visitor.route_to( destination_knot->second );
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
