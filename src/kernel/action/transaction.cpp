#include "core/id_factory.hpp"
#include "grab/context.hpp"
#include "grab/interaction.hpp"
#include "grab/locator.hpp"
#include "grab/query.hpp"
#include "grab/result.hpp"
#include "grab/trace.hpp"
#include "grab/ui.hpp"
#include "kernel/action/transaction.hpp"
#include "kernel/action/wait_engine.hpp"
#include "kernel/query/evaluator.hpp"
#include "kernel/query/snapshot_tree_nav.hpp"
#include "spi/event_source.hpp"
#include "spi/route.hpp"
#include "spi/runtime.hpp"
#include "spi/tree_source.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace grab::kernel::action
{

    namespace
    {

        [[nodiscard]]
        const ActionTarget&
        target_of( const Action& action )
        {
            return std::visit(
                []( const auto& verb ) -> const ActionTarget&
                {
                    return verb.target;
                },
                action
            );
        }

        [[nodiscard]]
        std::string
        match_target_string( const Match& match )
        {
            return std::string{ R"({"type":"match","runtime":)" } +
                   std::to_string( match.ref.runtime.value ) +
                   R"(,"tree":)" +
                   std::to_string( match.ref.tree ) +
                   R"(,"epoch":)" +
                   std::to_string( match.ref.epoch.value ) +
                   R"(,"node":)" +
                   std::to_string( match.ref.node ) +
                   R"(,"generation":)" +
                   std::to_string( match.ref.generation.value ) +
                   "}";
        }

        [[nodiscard]]
        std::string
        canonical_target( const ActionTarget& target )
        {
            if( const auto* const locator = std::get_if<Locator>( &target ) )
            {
                return locator->to_string();
            }
            return match_target_string( std::get<Match>( target ) );
        }

        [[nodiscard]]
        bool
        resolution_retryable( ErrorCode code ) noexcept
        {
            return code ==
                   ErrorCode::StaleNode ||
                   code ==
                   ErrorCode::TreeResynced ||
                   code ==
                   ErrorCode::RuntimeRestarted ||
                   code ==
                   ErrorCode::EnvironmentChanged ||
                   code ==
                   ErrorCode::TopologyChanged ||
                   code == ErrorCode::TargetDetached;
        }

        [[nodiscard]]
        bool
        route_allowed( RoutePolicy    policy,
                       spi::RouteKind kind ) noexcept
        {
            if( policy == RoutePolicy::SemanticOnly )
            {
                return kind == spi::RouteKind::Semantic;
            }
            if( policy == RoutePolicy::PhysicalOnly )
            {
                return kind == spi::RouteKind::Physical;
            }
            return true;
        }

        [[nodiscard]]
        std::vector<std::size_t>
        route_order( std::span<const spi::RouteDescriptor> routes,
                     RoutePolicy                           policy )
        {
            std::vector<std::size_t> ordered;
            ordered.reserve( routes.size() );
            const auto append_kind = [&ordered, routes, policy]( spi::RouteKind kind )
            {
                for( std::size_t index = 0U; index < routes.size(); ++index )
                {
                    if( routes[index].kind == kind && route_allowed( policy, kind ) )
                    {
                        ordered.push_back( index );
                    }
                }
            };
            append_kind( spi::RouteKind::Semantic );
            append_kind( spi::RouteKind::Physical );
            return ordered;
        }

        [[nodiscard]]
        Error
        route_unavailable_error( const Receipt& receipt )
        {
            Error error{
                .code        = ErrorCode::RouteUnavailable,
                .message     = "no action route could be reserved",
                .capability  = {},
                .target      = receipt.locator,
                .attempts    = {},
                .disposition = ErrorDisposition::Fatal,
                .diagnostics = {},
            };
            error.attempts.reserve( receipt.routes.size() );
            for( const auto& attempt : receipt.routes )
            {
                error.attempts.push_back( ProviderAttempt{
                    .provider = attempt.route,
                    .reason   = attempt.detail,
                } );
            }
            return error;
        }

        class TransactionRunner
        {
            public:

                TransactionRunner( spi::Runtime&             runtime,
                                   std::uint32_t             tree,
                                   const Action&             action,
                                   const ActionOptions&      options,
                                   const MappingRefreshHook& mapping_refresh,
                                   OperationContext          context ) :
                    runtime_{ &runtime },
                    tree_{ tree },
                    action_{ &action },
                    options_{ &options },
                    mapping_refresh_{ &mapping_refresh },
                    context_{ std::move( context ) }
                {
                    outcome_.receipt.operation = context_.operation;
                    outcome_.receipt.locator   = canonical_target( target_of( action ) );
                    outcome_.receipt.forced    = options.force;
                    outcome_.receipt.retry_class = options.retry;
                }

                [[nodiscard]]
                TransactionOutcome
                run()
                {
                    auto result = execute();
                    if( !result.has_value() )
                    {
                        outcome_.error = std::move( result.error() );
                        context_.note( std::string{ "action transaction failed: " } +
                                       outcome_.error->message );
                    }
                    neutralize();
                    outcome_.receipt.log = context_.log == nullptr
                                             ? std::vector<DiagnosticEntry>{}
                                             : context_.log->snapshot();
                    return std::move( outcome_ );
                }

            private:

                [[nodiscard]]
                Result<void>
                execute()
                {
                    auto checked = context_.check();
                    if( !checked.has_value() )
                    {
                        return checked;
                    }

                    auto resolved = resolve_target();
                    if( !resolved.has_value() )
                    {
                        return std::unexpected( std::move( resolved.error() ) );
                    }
                    target_                            = std::move( *resolved );
                    outcome_.receipt.snapshot_revision = target_->snapshot_revision;

                    auto mapping                       = refresh_mapping();
                    if( !mapping.has_value() )
                    {
                        return mapping;
                    }
                    if( !options_->force )
                    {
                        auto actionable = wait_for_actionability();
                        if( !actionable.has_value() )
                        {
                            return actionable;
                        }
                    }

                    auto reservation = reserve_route();
                    if( !reservation.has_value() )
                    {
                        return std::unexpected( std::move( reservation.error() ) );
                    }
                    return execute_reserved( **reservation );
                }

                [[nodiscard]]
                Result<Match>
                resolve_target()
                {
                    auto first = resolve_once();
                    if( first.has_value() ||
                        options_->retry ==
                        RetryClass::Never ||
                        !resolution_retryable( first.error().code ) )
                    {
                        return first;
                    }
                    auto* const events = runtime_->event_source();
                    if( events == nullptr )
                    {
                        return first;
                    }

                    std::optional<Match> resolved;
                    NamedPredicate       predicate{
                        .name    = "resolve_target",
                        .observe = [this, &resolved]() -> Result<PredicateObservation>
                        {
                            ++outcome_.receipt.resolve_retries;
                            auto retried = resolve_once();
                            if( retried.has_value() )
                            {
                                resolved = std::move( *retried );
                                return PredicateObservation{
                                    .satisfied = true,
                                    .detail    = "target resolved",
                                };
                            }
                            if( !resolution_retryable( retried.error().code ) )
                            {
                                return std::unexpected( std::move( retried.error() ) );
                            }
                            return PredicateObservation{
                                .satisfied = false,
                                .detail    = retried.error().message,
                            };
                        },
                    };
                    const WaitEngine engine{ context_ };
                    auto             waited =
                        engine.wait( predicate,
                                     WaitParams{ .deadline = context_.deadline },
                                     *events );
                    if( !waited.has_value() )
                    {
                        return std::unexpected( std::move( waited.error() ) );
                    }
                    return std::move( *resolved );
                }

                [[nodiscard]]
                Result<Match>
                resolve_once() const
                {
                    auto* const source = runtime_->tree_source();
                    if( source == nullptr )
                    {
                        return fail( ErrorCode::CapabilityUnavailable,
                                     "runtime has no tree source" );
                    }
                    const auto& action_target = target_of( *action_ );
                    if( const auto* const locator =
                            std::get_if<Locator>( &action_target ) )
                    {
                        auto snapshot = source->snapshot( tree_, context_ );
                        if( !snapshot.has_value() )
                        {
                            return std::unexpected( std::move( snapshot.error() ) );
                        }
                        const query::SnapshotTreeNav navigation{ *snapshot };
                        return query::resolve(
                            *locator,
                            options_->cardinality,
                            query::QueryScope{ .navigation = navigation }
                        );
                    }
                    return refresh_match( *source, std::get<Match>( action_target ) );
                }

                [[nodiscard]]
                Result<Match>
                refresh_match( spi::TreeSource& source,
                               const Match&     match ) const
                {
                    if( options_->cardinality == Cardinality::All )
                    {
                        return fail(
                            ErrorCode::InvalidArgument,
                            "single-target actions do not accept Cardinality::All"
                        );
                    }
                    auto snapshot = source.snapshot( match.ref.tree, context_ );
                    if( !snapshot.has_value() )
                    {
                        return std::unexpected( std::move( snapshot.error() ) );
                    }
                    const query::SnapshotTreeNav navigation{ *snapshot };
                    const auto                   metadata = navigation.metadata();
                    if( metadata.runtime != match.ref.runtime )
                    {
                        return fail( ErrorCode::RuntimeRestarted,
                                     "match runtime changed before action" );
                    }
                    if( metadata.tree !=
                        match.ref.tree ||
                        metadata.epoch != match.ref.epoch )
                    {
                        return fail( ErrorCode::TreeResynced,
                                     "match tree changed before action" );
                    }
                    const NodeId node{ match.ref.node };
                    if( !navigation.contains( node ) ||
                        navigation.generation( node ) != match.ref.generation )
                    {
                        return fail( ErrorCode::StaleNode,
                                     "match node changed before action" );
                    }
                    Match refreshed               = match;
                    refreshed.snapshot_revision   = metadata.revision;
                    refreshed.provenance.runtime  = metadata.runtime;
                    refreshed.provenance.revision = metadata.revision;
                    return refreshed;
                }

                [[nodiscard]]
                Result<void>
                refresh_mapping()
                {
                    if( !*mapping_refresh_ )
                    {
                        return {};
                    }
                    auto transforms = ( *mapping_refresh_ )( *target_, context_ );
                    if( !transforms.has_value() )
                    {
                        return std::unexpected( std::move( transforms.error() ) );
                    }
                    outcome_.receipt.transforms = std::move( *transforms );
                    return {};
                }

                [[nodiscard]]
                NodeObserver
                node_observer() const
                {
                    return [this]() -> Result<NodeObservation>
                    {
                        auto* const source = runtime_->tree_source();
                        if( source == nullptr )
                        {
                            return fail( ErrorCode::CapabilityUnavailable,
                                         "runtime has no tree source" );
                        }
                        auto snapshot = source->snapshot( target_->ref.tree, context_ );
                        if( !snapshot.has_value() )
                        {
                            return std::unexpected( std::move( snapshot.error() ) );
                        }
                        const query::SnapshotTreeNav navigation{ *snapshot };
                        const auto                   metadata = navigation.metadata();
                        const NodeId                 node{ target_->ref.node };
                        const bool identity_matches = metadata.runtime ==
                                                      target_->ref.runtime &&
                                                      metadata.tree ==
                                                      target_->ref.tree &&
                                                      metadata.epoch ==
                                                      target_->ref.epoch &&
                                                      navigation.contains( node ) &&
                                                      navigation.generation( node ) ==
                                                      target_->ref.generation;
                        return NodeObservation{
                            .present = identity_matches,
                            .states  = identity_matches ? navigation.states( node ) : 0U,
                            .detail  = identity_matches
                                         ? "resolved node observed"
                                         : "resolved node is stale or absent",
                        };
                    };
                }

                [[nodiscard]]
                Result<void>
                wait_for_actionability() const
                {
                    auto* const events = runtime_->event_source();
                    if( events == nullptr )
                    {
                        return fail( ErrorCode::CapabilityUnavailable,
                                     "runtime has no event source for actionability" );
                    }
                    const auto                  observer = node_observer();
                    std::vector<NamedPredicate> predicates;
                    predicates.push_back( node_present( observer ) );
                    predicates.push_back( state_stable( observer, 2U ) );
                    predicates.push_back( enabled( observer ) );
                    auto actionable = all_of( "actionable", std::move( predicates ) );
                    const WaitEngine engine{ context_ };
                    return engine.wait( actionable,
                                        WaitParams{ .deadline = context_.deadline },
                                        *events );
                }

                [[nodiscard]]
                spi::ActionRequest
                action_request() const
                {
                    return std::visit(
                        [this]( const auto& verb )
                        {
                            using Verb = std::decay_t<decltype( verb )>;
                            if constexpr( std::is_same_v<Verb, Click> )
                            {
                                return spi::ActionRequest{
                                    .verb   = spi::ActionVerb::Click,
                                    .target = *target_,
                                };
                            }
                            else if constexpr( std::is_same_v<Verb, TypeText> )
                            {
                                return spi::ActionRequest{
                                    .verb   = spi::ActionVerb::TypeText,
                                    .target = *target_,
                                    .text   = verb.text,
                                };
                            }
                            else if constexpr( std::is_same_v<Verb, Drag> )
                            {
                                return spi::ActionRequest{
                                    .verb         = spi::ActionVerb::Drag,
                                    .target       = *target_,
                                    .drag_from    = verb.from,
                                    .drag_to      = verb.to,
                                    .drag_options = verb.options,
                                };
                            }
                            else
                            {
                                return spi::ActionRequest{
                                    .verb     = spi::ActionVerb::PressKey,
                                    .target   = *target_,
                                    .key_name = verb.key_name,
                                };
                            }
                        },
                        *action_
                    );
                }

                [[nodiscard]]
                Result<std::unique_ptr<spi::RouteReservation>>
                reserve_route()
                {
                    const auto descriptors = runtime_->routes();
                    const auto order   = route_order( descriptors, options_->routing );
                    const auto request = action_request();
                    for( const auto index : order )
                    {
                        const auto&  descriptor = descriptors[index];
                        RouteAttempt attempt{
                            .route     = descriptor.name.empty()
                                           ? std::string{ "unnamed_route" }
                                           : std::string{ descriptor.name },
                            .selected  = false,
                            .rejection = ErrorCode::RouteUnavailable,
                            .detail    = {},
                        };
                        auto* const route = runtime_->action_route( index );
                        if( route == nullptr )
                        {
                            attempt.detail = "route has no action executor";
                            outcome_.receipt.routes.push_back( std::move( attempt ) );
                            continue;
                        }
                        auto reserved = route->reserve( request, context_ );
                        if( !reserved.has_value() )
                        {
                            attempt.rejection = reserved.error().code;
                            attempt.detail    = reserved.error().message;
                            outcome_.receipt.routes.push_back( std::move( attempt ) );
                            if( reserved.error().code == ErrorCode::PossiblyCommitted )
                            {
                                outcome_.receipt.commit =
                                    CommitStatus::PossiblyCommitted;
                                return std::unexpected( std::move( reserved.error() ) );
                            }
                            continue;
                        }
                        attempt.selected = true;
                        attempt.detail   = "selected";
                        outcome_.receipt.routes.push_back( std::move( attempt ) );
                        outcome_.receipt.fallback_used = options_->routing ==
                                                         RoutePolicy::PreferSemantic &&
                                                         descriptor.kind ==
                                                         spi::RouteKind::Physical;
                        return std::move( *reserved );
                    }
                    return std::unexpected(
                        route_unavailable_error( outcome_.receipt )
                    );
                }

                [[nodiscard]]
                Result<void>
                execute_reserved( spi::RouteReservation& reservation )
                {
                    for( const auto barrier : reservation.barriers() )
                    {
                        outcome_.receipt.barriers.push_back( BarrierOutcome{
                            .barrier   = std::string{ barrier },
                            .satisfied = false,
                            .timed_out = false,
                        } );
                        auto armed = reservation.arm_barrier( barrier, context_ );
                        if( !armed.has_value() )
                        {
                            return armed;
                        }
                    }

                    outcome_.receipt.commit = CommitStatus::PossiblyCommitted;
                    auto committed          = reservation.commit( context_ );
                    if( !committed.has_value() )
                    {
                        if( committed.error().code != ErrorCode::PossiblyCommitted )
                        {
                            outcome_.receipt.commit = CommitStatus::FailedBeforeCommit;
                        }
                        return committed;
                    }
                    outcome_.receipt.commit = CommitStatus::Committed;

                    auto settled            = reservation.settle( context_ );
                    if( !settled.has_value() )
                    {
                        if( settled.error().code == ErrorCode::DeadlineExceeded )
                        {
                            for( auto& barrier : outcome_.receipt.barriers )
                            {
                                if( !barrier.satisfied )
                                {
                                    barrier.timed_out = true;
                                }
                            }
                        }
                        return std::unexpected( std::move( settled.error() ) );
                    }
                    merge_barriers( *settled );

                    auto verified = reservation.verify( context_ );
                    if( !verified.has_value() )
                    {
                        return verified;
                    }
                    outcome_.receipt.commit = CommitStatus::Verified;
                    return {};
                }

                void
                merge_barriers( const std::vector<BarrierOutcome>& settled )
                {
                    for( const auto& observation : settled )
                    {
                        const auto found = std::ranges::find_if(
                            outcome_.receipt.barriers,
                            [&observation]( const BarrierOutcome& armed )
                            {
                                return armed.barrier == observation.barrier;
                            }
                        );
                        if( found == outcome_.receipt.barriers.end() )
                        {
                            outcome_.receipt.barriers.push_back( observation );
                        }
                        else
                        {
                            *found = observation;
                        }
                    }
                }

                void
                neutralize()
                {
                    auto* const seat = runtime_->input_seat();
                    if( seat == nullptr )
                    {
                        return;
                    }
                    auto result = seat->neutralize( context_ );
                    if( result.has_value() )
                    {
                        outcome_.receipt.neutralization = *result;
                        return;
                    }
                    outcome_.receipt.neutralization = NeutralizationOutcome::Failed;
                    context_.note( std::string{ "input neutralization failed: " } +
                                   result.error().message );
                    if( !outcome_.error.has_value() )
                    {
                        outcome_.error = Error{
                            .code        = ErrorCode::NeutralizationFailed,
                            .message     = result.error().message,
                            .capability  = {},
                            .target      = outcome_.receipt.locator,
                            .attempts    = {},
                            .disposition = ErrorDisposition::Fatal,
                            .diagnostics = result.error().diagnostics,
                        };
                    }
                }

                spi::Runtime*             runtime_{};
                std::uint32_t             tree_{};
                const Action*             action_{};
                const ActionOptions*      options_{};
                const MappingRefreshHook* mapping_refresh_{};
                OperationContext          context_{};
                TransactionOutcome        outcome_{};
                std::optional<Match>      target_;
        };

    }    // namespace

    Transaction::Transaction( spi::Runtime&      runtime,
                              std::uint32_t      tree,
                              MappingRefreshHook mapping_refresh ) :
        runtime_{ &runtime },
        tree_{ tree },
        mapping_refresh_{ std::move( mapping_refresh ) }
    {
    }

    TransactionOutcome
    Transaction::perform( const Action&        action,
                          const ActionOptions& options ) const
    {
        DiagnosticLog    log;
        OperationContext context{
            .deadline  = Deadline::after( options.deadline ),
            .stop      = options.stop,
            .operation = detail::next_operation_id(),
            .log       = &log,
        };
        TransactionRunner runner{
            *runtime_,
            tree_,
            action,
            options,
            mapping_refresh_,
            std::move( context )
        };
        return runner.run();
    }

}    // namespace grab::kernel::action
