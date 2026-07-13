#include "grab/context.hpp"
#include "grab/result.hpp"
#include "grab/ui.hpp"
#include "kernel/action/wait_engine.hpp"
#include "spi/event_source.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace grab::kernel::action
{

    namespace
    {

        struct StabilityState
        {
                std::optional<std::uint32_t> previous;
                std::size_t                  consecutive{};
        };

        [[nodiscard]]
        Deadline
        earliest( Deadline left,
                  Deadline right ) noexcept
        {
            return Deadline{ .at = std::min( left.at, right.at ) };
        }

        [[nodiscard]]
        Result<void>
        timeout( const OperationContext& context,
                 std::string_view        predicate,
                 std::string_view        last_failure )
        {
            const std::string diagnostic = std::string{ "predicate '" } +
                                           std::string{ predicate } +
                                           "' last failure: " +
                                           std::string{ last_failure };
            context.note( diagnostic );
            Error error{
                .code        = ErrorCode::DeadlineExceeded,
                .message     = std::string{ "wait for predicate '" }
                      +
                               std::string{ predicate }
                      +
                               "' exceeded its deadline",
                .capability  = {},
                .target      = {},
                .attempts    = {},
                .disposition = ErrorDisposition::Fatal,
                .diagnostics = {
                                           DiagnosticEntry{
                        .at      = std::chrono::steady_clock::now(),
                        .message = diagnostic,
                    }, },
            };
            return std::unexpected( std::move( error ) );
        }

        [[nodiscard]]
        Result<void>
        finish_wait( spi::EventSource&       event_source,
                     const spi::EventSpec&   spec,
                     const OperationContext& context,
                     Result<void>            result )
        {
            auto disabled = event_source.disable( spec );
            if( !disabled.has_value() )
            {
                context.note( std::string{ "failed to disable wait event '" } +
                              spec.name +
                              "': " +
                              disabled.error().message );
                if( result.has_value() )
                {
                    return std::unexpected( std::move( disabled.error() ) );
                }
            }
            return result;
        }

        [[nodiscard]]
        std::chrono::nanoseconds
        next_backoff( std::chrono::nanoseconds current,
                      std::chrono::nanoseconds maximum ) noexcept
        {
            if( current >= maximum || current > maximum / 2 )
            {
                return maximum;
            }
            return std::min( current * 2, maximum );
        }

    }    // namespace

    NamedPredicate
    node_present( NodeObserver observer )
    {
        return NamedPredicate{
            .name    = "node_present",
            .observe = [observer =
                            std::move( observer )]() -> Result<PredicateObservation>
            {
                auto observed = observer();
                if( !observed.has_value() )
                {
                    return std::unexpected( std::move( observed.error() ) );
                }
                return PredicateObservation{
                    .satisfied = observed->present,
                    .detail = observed->present ? "node is present" : observed->detail,
                };
            },
        };
    }

    NamedPredicate
    enabled( NodeObserver observer )
    {
        return NamedPredicate{
            .name    = "enabled",
            .observe = [observer =
                            std::move( observer )]() -> Result<PredicateObservation>
            {
                auto observed = observer();
                if( !observed.has_value() )
                {
                    return std::unexpected( std::move( observed.error() ) );
                }
                const bool is_enabled =
                    observed->present &&
                    has_state( observed->states, NodeState::Enabled );
                return PredicateObservation{
                    .satisfied = is_enabled,
                    .detail    = is_enabled ? "node is enabled"
                                            : "node is not enabled: " + observed->detail,
                };
            },
        };
    }

    NamedPredicate
    state_stable( NodeObserver observer,
                  std::size_t  required_observations )
    {
        auto state = std::make_shared<StabilityState>();
        return NamedPredicate{
            .name    = "state_stable",
            .observe = [observer = std::move( observer ),
                        required_observations,
                        state =
                            std::move( state )]() mutable -> Result<PredicateObservation>
            {
                if( required_observations == 0U )
                {
                    return fail( ErrorCode::InvalidArgument,
                                 "state stability requires at least one observation" );
                }
                auto observed = observer();
                if( !observed.has_value() )
                {
                    return std::unexpected( std::move( observed.error() ) );
                }
                if( !observed->present )
                {
                    state->previous.reset();
                    state->consecutive = 0U;
                    return PredicateObservation{
                        .satisfied = false,
                        .detail    = "node disappeared while observing stability",
                    };
                }
                if( state->previous == observed->states )
                {
                    ++state->consecutive;
                }
                else
                {
                    state->previous    = observed->states;
                    state->consecutive = 1U;
                }
                const bool stable = state->consecutive >= required_observations;
                return PredicateObservation{
                    .satisfied = stable,
                    .detail = stable ? "node state is stable"
                                     : "node state has not been observed enough times",
                };
            },
        };
    }

    NamedPredicate
    all_of( std::string                 name,
            std::vector<NamedPredicate> predicates )
    {
        return NamedPredicate{
            .name    = std::move( name ),
            .observe = [predicates = std::move( predicates )]() mutable
                -> Result<PredicateObservation>
            {
                for( auto& predicate : predicates )
                {
                    if( !predicate.observe )
                    {
                        return fail( ErrorCode::InvalidArgument,
                                     "composed predicate has no observer" );
                    }
                    auto observed = predicate.observe();
                    if( !observed.has_value() )
                    {
                        return std::unexpected( std::move( observed.error() ) );
                    }
                    if( !observed->satisfied )
                    {
                        return PredicateObservation{
                            .satisfied = false,
                            .detail    = predicate.name + ": " + observed->detail,
                        };
                    }
                }
                return PredicateObservation{
                    .satisfied = true,
                    .detail    = "all checks satisfied",
                };
            },
        };
    }

    WaitEngine::WaitEngine( OperationContext context ) :
        context_{ std::move( context ) }
    {
    }

    Result<void>
    WaitEngine::wait( NamedPredicate&   predicate,
                      const WaitParams& params,
                      spi::EventSource& event_source ) const
    {
        if( predicate.name.empty() || !predicate.observe )
        {
            return fail( ErrorCode::InvalidArgument,
                         "wait predicate must have a name and observer" );
        }
        if( params.backoff.initial <=
            std::chrono::nanoseconds::zero() ||
            params.backoff.maximum < params.backoff.initial )
        {
            return fail( ErrorCode::InvalidArgument,
                         "wait backoff must be positive and bounded" );
        }

        const spi::EventSpec spec{ .name = predicate.name };
        auto                 enabled_result = event_source.enable( spec );
        if( !enabled_result.has_value() )
        {
            return std::unexpected( std::move( enabled_result.error() ) );
        }

        OperationContext wait_context = context_;
        wait_context.deadline         = earliest( context_.deadline, params.deadline );
        auto        backoff           = params.backoff.initial;
        std::string last_failure      = "predicate was not satisfied";

        while( true )
        {
            if( wait_context.stop.stop_requested() )
            {
                return finish_wait( event_source,
                                    spec,
                                    wait_context,
                                    fail( ErrorCode::Cancelled,
                                          "operation cancelled" ) );
            }

            auto observed = predicate.observe();
            if( !observed.has_value() )
            {
                return finish_wait( event_source,
                                    spec,
                                    wait_context,
                                    std::unexpected( std::move( observed.error() ) ) );
            }
            if( observed->satisfied )
            {
                return finish_wait( event_source, spec, wait_context, {} );
            }
            last_failure = observed->detail;

            if( wait_context.deadline.expired() )
            {
                return finish_wait(
                    event_source,
                    spec,
                    wait_context,
                    timeout( wait_context, predicate.name, last_failure )
                );
            }

            const auto maximum_wait =
                std::min( backoff, wait_context.deadline.remaining() );
            auto wake_result =
                event_source.wait_for_event( spec, wait_context, maximum_wait );
            if( !wake_result.has_value() )
            {
                if( wake_result.error().code == ErrorCode::DeadlineExceeded )
                {
                    return finish_wait(
                        event_source,
                        spec,
                        wait_context,
                        timeout( wait_context, predicate.name, last_failure )
                    );
                }
                return finish_wait(
                    event_source,
                    spec,
                    wait_context,
                    std::unexpected( std::move( wake_result.error() ) )
                );
            }
            backoff = next_backoff( backoff, params.backoff.maximum );
        }
    }

}    // namespace grab::kernel::action
