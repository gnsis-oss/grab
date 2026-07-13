#include "core/environment.hpp"
#include "grab/capability.hpp"
#include "grab/result.hpp"
#include "grab/session.hpp"
#include "session/provider.hpp"
#include "session/selection.hpp"

#include <array>
#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace grab::session
{
    namespace
    {

        struct Candidate
        {
                const SessionProvider* provider = nullptr;
                Availability           availability;
        };

        [[nodiscard]]
        int
        rank( AvailabilityState state ) noexcept
        {
            switch( state )
            {
                case AvailabilityState::Available :
                    return 3;
                case AvailabilityState::Degraded :
                    return 2;
                case AvailabilityState::NeedsPermission :
                    return 1;
                case AvailabilityState::Count :
                case AvailabilityState::Unavailable :
                    return 0;
            }
            return 0;
        }

        [[nodiscard]]
        bool
        is_better( const Availability& lhs,
                   const Availability& rhs ) noexcept
        {
            const int lhs_rank = rank( lhs.state );
            const int rhs_rank = rank( rhs.state );
            if( lhs_rank != rhs_rank )
            {
                return lhs_rank > rhs_rank;
            }
            return lhs.quality > rhs.quality;
        }

        [[nodiscard]]
        std::vector<ProviderAttempt>
        attempts_for( std::span<const Candidate> candidates )
        {
            std::vector<ProviderAttempt> attempts;
            attempts.reserve( candidates.size() );
            for( const Candidate& candidate : candidates )
            {
                attempts.push_back( ProviderAttempt{
                    .provider = candidate.provider->info().name,
                    .reason   = candidate.availability.reason,
                } );
            }
            return attempts;
        }

        [[nodiscard]]
        std::string
        no_provider_message( SessionMode                mode,
                             std::span<const Candidate> candidates )
        {
            std::string message  = "no session provider for mode ";
            message             += mode_name( mode );
            for( const Candidate& candidate : candidates )
            {
                message += "; ";
                message += candidate.provider->info().name;
                message += ": ";
                if( candidate.availability.reason.empty() )
                {
                    message += state_name( candidate.availability.state );
                }
                else
                {
                    message += candidate.availability.reason;
                }
            }
            return message;
        }

        [[nodiscard]]
        std::vector<Candidate>
        probe_providers( std::span<const SessionProvider* const> providers,
                         const core::Environment&                env,
                         SessionMode                             mode )
        {
            std::vector<Candidate> candidates;
            candidates.reserve( providers.size() );
            for( const SessionProvider* provider : providers )
            {
                candidates.push_back( Candidate{
                    .provider     = provider,
                    .availability = provider->probe( env, mode ),
                } );
            }
            return candidates;
        }

        [[nodiscard]]
        const Candidate*
        best_candidate( std::span<const Candidate> candidates ) noexcept
        {
            if( candidates.empty() )
            {
                return nullptr;
            }

            const Candidate* best = &candidates.front();
            for( const Candidate& candidate : candidates.subspan( 1U ) )
            {
                if( is_better( candidate.availability, best->availability ) )
                {
                    best = &candidate;
                }
            }
            return best;
        }

    }    // namespace

    grab::Result<const SessionProvider*>
    select_session_provider( std::span<const SessionProvider* const> providers,
                             const grab::core::Environment&          env,
                             grab::SessionMode                       mode )
    {
        const std::vector<Candidate> candidates =
            probe_providers( providers, env, mode );
        const Candidate* best = best_candidate( candidates );
        if( best == nullptr || rank( best->availability.state ) == 0 )
        {
            return std::unexpected( Error{
                .code       = ErrorCode::CapabilityUnavailable,
                .message    = no_provider_message( mode, candidates ),
                .capability = {},
                .target     = std::string{ mode_name( mode ) },
                .attempts   = attempts_for( candidates ),
            } );
        }

        return best->provider;
    }

    std::vector<SessionModeReport>
    session_availability_report( std::span<const SessionProvider* const> providers,
                                 const grab::core::Environment&          env )
    {
        constexpr auto modes = std::to_array<SessionMode>( {
            SessionMode::Shared,
            SessionMode::Offscreen,
        } );
        static_assert( modes.size() == static_cast<std::size_t>( SessionMode::Count ) );

        std::vector<SessionModeReport> report;
        report.reserve( modes.size() );
        for( const SessionMode mode : modes )
        {
            const std::vector<Candidate> candidates =
                probe_providers( providers, env, mode );
            const Candidate* best = best_candidate( candidates );
            if( best == nullptr )
            {
                report.push_back( SessionModeReport{
                    .mode     = mode,
                    .provider = {},
                    .state    = AvailabilityState::Unavailable,
                    .reason   = "no provider",
                } );
                continue;
            }

            report.push_back( SessionModeReport{
                .mode     = mode,
                .provider = best->provider->info().name,
                .state    = best->availability.state,
                .reason   = best->availability.reason,
            } );
        }
        return report;
    }

}    // namespace grab::session
