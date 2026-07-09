#include "core/environment.hpp"
#include "core/log.hpp"
#include "core/provider.hpp"
#include "core/registry.hpp"
#include "core/resolver.hpp"
#include "grab/capability.hpp"
#include "grab/result.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace grab::core
{

    namespace
    {

        [[nodiscard]]
        int
        rank( AvailabilityState state,
              bool              prefer_permission_over_degraded ) noexcept
        {
            switch( state )
            {
                case AvailabilityState::available :
                    return 0;
                case AvailabilityState::degraded :
                    return prefer_permission_over_degraded ? 2 : 1;
                case AvailabilityState::needs_permission :
                    // An exact provider pending one persistable consent beats
                    // permanently degraded output; this matches spec require()
                    // semantics.
                    return prefer_permission_over_degraded ? 1 : 2;
                case AvailabilityState::count :
                case AvailabilityState::unavailable :
                    return 3;
            }
            return 3;
        }

        void
        log_cache( std::string_view         result,
                   const CapabilityRequest& request,
                   const Environment&       env )
        {
            log::verbose(
                [result, &request, &env]( auto& event )
                {
                    event.tag( "resolver.cache" )
                        .value( "result", result )
                        .value( "generation", env.generation )
                        .value( "capability", capability_name( request.capability ) )
                        .value( "target_class", request.target_class )
                        .value( "target_key", request.target_key );
                }
            );
        }

        void
        log_cached_outcome( const CapabilityRequest& request,
                            const Resolution&        resolution )
        {
            log::nominal(
                [&request, &resolution]( auto& event )
                {
                    event.tag( "resolver.outcome" )
                        .value( "result", "cached" )
                        .value( "capability", capability_name( request.capability ) )
                        .value( "chain", resolution.chain.size() )
                        .value( "state", state_name( resolution.best.state ) );
                }
            );
        }

        void
        log_probe_result( const CapabilityRequest& request,
                          const Provider&          provider,
                          const Availability&      availability )
        {
            log::verbose(
                [&request, &provider, &availability]( auto& event )
                {
                    event.tag( "resolver.probe" )
                        .value( "provider", provider.info().name )
                        .value( "capability", capability_name( request.capability ) )
                        .value( "state", state_name( availability.state ) )
                        .value( "quality", availability.quality );
                }
            );
        }

        void
        log_unavailable_outcome( std::string_view capability_id,
                                 std::size_t      chain_size,
                                 std::size_t      attempt_count )
        {
            log::nominal(
                [capability_id, chain_size, attempt_count]( auto& event )
                {
                    event.tag( "resolver.outcome" )
                        .value( "result", "unavailable" )
                        .value( "capability", capability_id )
                        .value( "chain", chain_size )
                        .value( "attempts", attempt_count );
                }
            );
        }

        void
        log_resolved_outcome( const CapabilityRequest& request,
                              const Resolution&        resolution )
        {
            log::nominal(
                [&request, &resolution]( auto& event )
                {
                    event.tag( "resolver.outcome" )
                        .value( "result", "resolved" )
                        .value( "capability", capability_name( request.capability ) )
                        .value( "chain", resolution.chain.size() )
                        .value( "state", state_name( resolution.best.state ) );
                }
            );
        }

    }    // namespace

    Resolver::Resolver( const Registry& registry ) :
        registry_( registry )
    {
    }

    Result<Resolution>
    Resolver::resolve( const CapabilityRequest& request,
                       const Environment&       env ) const
    {
        const CacheKey key{
            .generation   = env.generation,
            .capability   = request.capability,
            .target_class = request.target_class,
            .target_key   = request.target_key,
            .prefer_permission_over_degraded =
                request.options.prefer_permission_over_degraded,
        };
        const std::scoped_lock lock( mutex_ );

        // Resolution is cold-path; the lock intentionally serializes probing so
        // a cache generation never observes a second probe. Per-key in-flight
        // tracking can replace this if Phase 2 needs concurrent resolution.
        if( const auto found = cache_.find( key ); found != cache_.end() )
        {
            log_cache( "hit", request, env );
            log_cached_outcome( request, found->second );
            return found->second;
        }
        log_cache( "miss", request, env );

        struct Candidate
        {
                const Provider* provider;
                Availability    availability;
        };

        std::vector<Candidate>       candidates;
        std::vector<ProviderAttempt> rejected;

        for( const auto* provider : registry_.providers_for( request.capability ) )
        {
            auto availability = provider->probe( env );
            log_probe_result( request, *provider, availability );
            if( availability.state == AvailabilityState::unavailable )
            {
                rejected.push_back( ProviderAttempt{
                    .provider = provider->info().name,
                    .reason   = availability.reason,
                } );
                continue;
            }
            candidates.push_back( Candidate{
                .provider     = provider,
                .availability = std::move( availability ),
            } );
        }

        if( candidates.empty() )
        {
            const std::string capability_id{ capability_name( request.capability ) };
            log_unavailable_outcome( capability_id, candidates.size(), rejected.size() );
            Error error{
                .code       = ErrorCode::capability_unavailable,
                .message    = "no provider available for " + capability_id,
                .capability = capability_id,
                .target     = request.target_key.empty() ? request.target_class
                                                         : request.target_key,
                .attempts   = std::move( rejected ),
            };
            return std::unexpected( std::move( error ) );
        }

        const bool prefer_permission_over_degraded =
            request.options.prefer_permission_over_degraded;
        std::ranges::stable_sort(
            candidates,
            [prefer_permission_over_degraded]( const auto& lhs, const auto& rhs )
            {
                const int rank_lhs =
                    rank( lhs.availability.state, prefer_permission_over_degraded );
                const int rank_rhs =
                    rank( rhs.availability.state, prefer_permission_over_degraded );
                if( rank_lhs != rank_rhs )
                {
                    return rank_lhs < rank_rhs;
                }
                return lhs.availability.quality > rhs.availability.quality;
            }
        );

        Resolution resolution;
        resolution.best = candidates.front().availability;
        for( const auto& candidate : candidates )
        {
            resolution.chain.push_back( candidate.provider );
        }

        cache_.emplace( key, resolution );
        log_resolved_outcome( request, resolution );
        return resolution;
    }

}    // namespace grab::core
