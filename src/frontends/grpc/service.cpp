#include "codec/png.hpp"
#include "eventgrab/v1/events.pb.h"
#include "eventgrab/v1/service.pb.h"
#include "frontends/grpc/codec.hpp"
#include "frontends/grpc/proto_descriptor.hpp"
#include "frontends/grpc/service.hpp"
#include "grab/active_kind_probe.hpp"
#include "grab/capture.hpp"
#include "grab/command_descriptor.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/event_descriptor.hpp"
#include "grab/ids.hpp"
#include "grab/interaction.hpp"
#include "grab/locator.hpp"
#include "grab/process_ref.hpp"
#include "grab/result.hpp"
#include "grab/session.hpp"
#include "grab/trace.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <expected>
#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>
#include <grpcpp/support/status_code_enum.h>
#include <grpcpp/support/sync_stream.h>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace grab::transport
{
    namespace
    {

        constexpr auto admissionPollInterval        = std::chrono::milliseconds{ 2 };
        constexpr auto ownedProcessTerminationGrace = std::chrono::milliseconds{ 250 };
        constexpr std::string_view queueFullReason{
            "admission rejected: concurrency cap reached and bounded queue full",
        };
        constexpr std::string_view admissionDeadlineReason{
            "admission rejected: per-call deadline expired",
        };
        constexpr std::string_view grabErrorDetailPrefix{ "grab-error: " };

        constexpr auto registeredRpcNames = std::to_array<std::string_view>( {
            "PushEvent",
            "ListEventTypes",
            "Subscribe",
            "SetClientContext",
            "ResolveNode",
            "PerformAction",
            "CaptureFrame",
        } );

        struct NotifyState
        {
                std::mutex              mutex;
                std::condition_variable data_ready;
                bool                    notified = false;
        };

        [[nodiscard]]
        grpc::Status
        invalid_argument( std::string_view message )
        {
            return grpc::Status{
                grpc::StatusCode::INVALID_ARGUMENT,
                std::string{ message },
            };
        }

        [[nodiscard]]
        grpc::Status
        internal_error( std::string_view message )
        {
            return grpc::Status{ grpc::StatusCode::INTERNAL, std::string{ message } };
        }

        [[nodiscard]]
        grpc::Status
        status_from_error( const grab::Error& error )
        {
            grpc::StatusCode code = grpc::StatusCode::FAILED_PRECONDITION;
            switch( error.code )
            {
                case grab::ErrorCode::InvalidArgument :
                    code = grpc::StatusCode::INVALID_ARGUMENT;
                    break;
                case grab::ErrorCode::NoMatch :
                    code = grpc::StatusCode::NOT_FOUND;
                    break;
                case grab::ErrorCode::DeadlineExceeded :
                    code = grpc::StatusCode::DEADLINE_EXCEEDED;
                    break;
                case grab::ErrorCode::Cancelled :
                    code = grpc::StatusCode::CANCELLED;
                    break;
                case grab::ErrorCode::PermissionNeeded :
                case grab::ErrorCode::PermissionDenied :
                    code = grpc::StatusCode::PERMISSION_DENIED;
                    break;
                case grab::ErrorCode::InternalFault :
                    code = grpc::StatusCode::INTERNAL;
                    break;
                default :
                    break;
            }
            return grpc::Status{
                code,
                error.message,
                std::string{ grabErrorDetailPrefix } +
                    std::string{ grab::name_of( error.code ) },
            };
        }

        [[nodiscard]]
        grab::Error
        capability_error( std::string_view message )
        {
            return grab::Error{
                .code        = grab::ErrorCode::CapabilityUnavailable,
                .message     = std::string{ message },
                .capability  = {},
                .target      = {},
                .attempts    = {},
                .disposition = grab::ErrorDisposition::Fatal,
                .diagnostics = {},
            };
        }

        [[nodiscard]]
        std::string
        known_command_names()
        {
            std::string names;
            for( const auto& descriptor : grab::list_commands() )
            {
                if( !names.empty() )
                {
                    names.append( ", " );
                }
                names.append( descriptor.name );
            }
            return names;
        }

        [[nodiscard]]
        std::optional<grab::Cardinality>
        decode_cardinality( std::uint32_t value ) noexcept
        {
            switch( value )
            {
                case 0U :
                    return grab::Cardinality::ExactlyOne;
                case 1U :
                    return grab::Cardinality::First;
                case 2U :
                    return grab::Cardinality::All;
                default :
                    return std::nullopt;
            }
        }

        [[nodiscard]]
        std::optional<grab::RoutePolicy>
        decode_routing( std::uint32_t value ) noexcept
        {
            switch( value )
            {
                case 0U :
                    return grab::RoutePolicy::PreferSemantic;
                case 1U :
                    return grab::RoutePolicy::SemanticOnly;
                case 2U :
                    return grab::RoutePolicy::PhysicalOnly;
                default :
                    return std::nullopt;
            }
        }

        [[nodiscard]]
        std::optional<grab::RetryClass>
        decode_retry( std::uint32_t value ) noexcept
        {
            switch( value )
            {
                case 0U :
                    return grab::RetryClass::Never;
                case 1U :
                    return grab::RetryClass::ResolveOnly;
                case 2U :
                    return grab::RetryClass::Idempotent;
                case 3U :
                    return grab::RetryClass::Compensated;
                default :
                    return std::nullopt;
            }
        }

        [[nodiscard]]
        std::uint32_t
        encode_consistency( grab::ConsistencyMode mode ) noexcept
        {
            switch( mode )
            {
                case grab::ConsistencyMode::Live :
                    return 0U;
                case grab::ConsistencyMode::Revisioned :
                    return 1U;
                case grab::ConsistencyMode::Pinned :
                    return 2U;
            }
            return 0U;
        }

        void
        encode_match( const grab::Match&        match,
                      eventgrab::v1::MatchWire* wire )
        {
            auto* ref = wire->mutable_ref();
            ref->set_runtime( match.ref.runtime.value );
            ref->set_tree( match.ref.tree );
            ref->set_epoch( match.ref.epoch.value );
            ref->set_node( match.ref.node );
            ref->set_generation( match.ref.generation.value );
            wire->set_consistency( encode_consistency( match.mode ) );
            wire->set_snapshot_revision( match.snapshot_revision );
            for( const auto& predicate : match.matched_predicates )
            {
                wire->add_matched_predicates( predicate );
            }
            wire->set_provider( match.provenance.provider );
            wire->set_candidate_provider( match.provenance.candidate_provider );
            wire->set_provenance_runtime( match.provenance.runtime.value );
            wire->set_provenance_revision( match.provenance.revision );
        }

        [[nodiscard]]
        grab::WidgetRef
        decode_widget_ref( const eventgrab::v1::WidgetRefWire& wire ) noexcept
        {
            return grab::WidgetRef{
                .runtime    = grab::RuntimeId{ wire.runtime() },
                .tree       = wire.tree(),
                .epoch      = grab::TreeEpoch{ wire.epoch() },
                .node       = wire.node(),
                .generation = grab::NodeGeneration{ wire.generation() },
            };
        }

        [[nodiscard]]
        grpc::Status
        unauthenticated( std::string_view message )
        {
            return grpc::Status{
                grpc::StatusCode::UNAUTHENTICATED,
                std::string{ message }
            };
        }

        [[nodiscard]]
        std::string
        make_session_value( std::string_view prefix,
                            std::uint64_t    sequence )
        {
            std::random_device random;
            std::ostringstream value;
            value << prefix << '-' << std::hex << sequence << '-'
                  << static_cast<std::uint64_t>( random() ) << '-'
                  << static_cast<std::uint64_t>( random() );
            return value.str();
        }

        [[nodiscard]]
        bool
        token_matches( std::string_view expected,
                       std::string_view supplied ) noexcept
        {
            std::size_t difference = expected.size() ^ supplied.size();
            const auto  length     = std::max( expected.size(), supplied.size() );
            for( std::size_t index = 0U; index < length; ++index )
            {
                const auto left   = index < expected.size()
                                      ? static_cast<unsigned char>( expected[index] )
                                      : static_cast<unsigned char>( 0U );
                const auto right  = index < supplied.size()
                                      ? static_cast<unsigned char>( supplied[index] )
                                      : static_cast<unsigned char>( 0U );
                difference       |= static_cast<std::size_t>( left ^ right );
            }
            return difference == 0U;
        }

        [[nodiscard]]
        grab::Result<grab::EventKind>
        from_wire_filter_kind( eventgrab::v1::EventKind kind )
        {
            const auto grab_kind = grab::transport::to_grab_kind( kind );
            if( !grab_kind.has_value() || *grab_kind == grab::EventKind::Unspecified )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "invalid event filter kind" );
            }
            return *grab_kind;
        }

        [[nodiscard]]
        grab::Result<grab::EventCategory>
        from_wire_filter_category( eventgrab::v1::EventCategory category )
        {
            const auto grab_category = grab::transport::to_grab_category( category );
            if( !grab_category.has_value() )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "invalid event filter category" );
            }
            return *grab_category;
        }

        [[nodiscard]]
        grab::Result<grab::EventFilter>
        from_wire_filter( const eventgrab::v1::EventFilter& wire )
        {
            grab::EventFilter filter;
            filter.kinds.reserve( static_cast<std::size_t>( wire.kinds_size() ) );
            filter.categories.reserve(
                static_cast<std::size_t>( wire.categories_size() )
            );

            for( const int kind_value : wire.kinds() )
            {
                auto kind = from_wire_filter_kind(
                    static_cast<eventgrab::v1::EventKind>( kind_value )
                );
                if( !kind.has_value() )
                {
                    return std::unexpected( kind.error() );
                }
                filter.kinds.push_back( *kind );
            }

            for( const int category_value : wire.categories() )
            {
                auto category = from_wire_filter_category(
                    static_cast<eventgrab::v1::EventCategory>( category_value )
                );
                if( !category.has_value() )
                {
                    return std::unexpected( category.error() );
                }
                filter.categories.push_back( *category );
            }

            return filter;
        }

        void
        notify_waiter( const std::shared_ptr<NotifyState>& state )
        {
            {
                const std::scoped_lock lock( state->mutex );
                state->notified = true;
            }
            state->data_ready.notify_one();
        }

        void
        wait_for_data_or_poll_interval( const std::shared_ptr<NotifyState>& state,
                                        std::chrono::milliseconds poll_interval )
        {
            std::unique_lock lock( state->mutex );
            if( !state->notified )
            {
                state->data_ready.wait_for( lock,
                                            poll_interval,
                                            [&]
                                            {
                                                return state->notified;
                                            } );
            }
            state->notified = false;
        }

        [[nodiscard]]
        grpc::Status
        write_available_events( const grpc::ServerContext&                context,
                                grpc::ServerWriter<eventgrab::v1::Event>& writer,
                                grab::Subscription&                       subscription )
        {
            while( true )
            {
                auto event = subscription.try_pop();
                if( !event.has_value() )
                {
                    return grpc::Status::OK;
                }

                auto wire = grab::transport::to_wire( *event );
                if( !wire.has_value() )
                {
                    return internal_error( wire.error().message );
                }

                if( !writer.Write( *wire ) || context.IsCancelled() )
                {
                    return grpc::Status{
                        grpc::StatusCode::CANCELLED,
                        "event stream cancelled"
                    };
                }
            }
        }

    }    // namespace

    class EventService::AdmissionController
    {
        public:

            explicit AdmissionController( AdmissionPolicy policy ) :
                policy_( policy )
            {
            }

            [[nodiscard]]
            grpc::Status
            run( grpc::ServerContext*                 context,
                 const std::function<grpc::Status()>& work )
            {
                const auto admission = acquire( context );
                if( !admission.ok() )
                {
                    return admission;
                }

                struct SlotRelease
                {
                        AdmissionController* controller;

                        ~SlotRelease()
                        {
                            controller->release();
                        }
                };

                const SlotRelease release{ this };
                return work();
            }

        private:

            [[nodiscard]]
            bool
            healthy() const noexcept
            {
                return policy_.healthy ==
                       nullptr ||
                       policy_.healthy->load( std::memory_order_acquire );
            }

            [[nodiscard]]
            grpc::Status
            acquire( grpc::ServerContext* context )
            {
                if( !healthy() )
                {
                    return grpc::Status{
                        grpc::StatusCode::UNAVAILABLE,
                        std::string{ policy_.unhealthy_reason }
                    };
                }

                std::unique_lock lock( mutex_ );
                if( active_ < policy_.concurrency_cap && queue_.empty() )
                {
                    ++active_;
                    return grpc::Status::OK;
                }
                if( queue_.size() >= policy_.queue_capacity )
                {
                    return grpc::Status{
                        grpc::StatusCode::RESOURCE_EXHAUSTED,
                        std::string{ queueFullReason }
                    };
                }

                const auto ticket = next_ticket_++;
                queue_.push_back( ticket );
                const auto deadline =
                    std::chrono::steady_clock::now() + policy_.per_call_deadline;

                while( true )
                {
                    if( !healthy() )
                    {
                        erase_ticket( ticket );
                        return grpc::Status{
                            grpc::StatusCode::UNAVAILABLE,
                            std::string{ policy_.unhealthy_reason }
                        };
                    }
                    if( context != nullptr && context->IsCancelled() )
                    {
                        erase_ticket( ticket );
                        return grpc::Status{
                            grpc::StatusCode::CANCELLED,
                            "admission cancelled"
                        };
                    }
                    if( !queue_.empty() &&
                        queue_.front() ==
                        ticket &&
                        active_ < policy_.concurrency_cap )
                    {
                        queue_.pop_front();
                        ++active_;
                        return grpc::Status::OK;
                    }
                    if( std::chrono::steady_clock::now() >= deadline )
                    {
                        erase_ticket( ticket );
                        return grpc::Status{
                            grpc::StatusCode::DEADLINE_EXCEEDED,
                            std::string{ admissionDeadlineReason }
                        };
                    }
                    condition_.wait_for( lock, admissionPollInterval );
                }
            }

            void
            erase_ticket( std::uint64_t ticket )
            {
                const auto entry = std::find( queue_.begin(), queue_.end(), ticket );
                if( entry != queue_.end() )
                {
                    queue_.erase( entry );
                    condition_.notify_all();
                }
            }

            void
            release() noexcept
            {
                {
                    const std::scoped_lock lock( mutex_ );
                    if( active_ > 0U )
                    {
                        --active_;
                    }
                }
                condition_.notify_all();
            }

            AdmissionPolicy           policy_;
            std::mutex                mutex_;
            std::condition_variable   condition_;
            std::deque<std::uint64_t> queue_;
            std::uint64_t             next_ticket_{};
            std::size_t               active_{};
    };

    PeerSessionRegistry::CatalogScope::CatalogScope(
        PeerSessionRegistry&            registry,
        std::string                     peer,
        std::unordered_set<std::string> snapshot
    ) noexcept :
        registry_( &registry ),
        peer_( std::move( peer ) ),
        snapshot_( std::move( snapshot ) )
    {
    }

    PeerSessionRegistry::CatalogScope::CatalogScope( CatalogScope&& other ) noexcept :
        registry_( std::exchange( other.registry_,
                                  nullptr ) ),
        peer_( std::move( other.peer_ ) ),
        snapshot_( std::move( other.snapshot_ ) )
    {
    }

    PeerSessionRegistry::CatalogScope&
    PeerSessionRegistry::CatalogScope::operator=( CatalogScope&& other ) noexcept
    {
        if( this != &other )
        {
            finish();
            registry_ = std::exchange( other.registry_, nullptr );
            peer_     = std::move( other.peer_ );
            snapshot_ = std::move( other.snapshot_ );
        }
        return *this;
    }

    PeerSessionRegistry::CatalogScope::~CatalogScope()
    {
        finish();
    }

    void
    PeerSessionRegistry::CatalogScope::finish() noexcept
    {
        if( registry_ != nullptr )
        {
            registry_->reap_diff( peer_, snapshot_ );
            registry_ = nullptr;
        }
    }

    PeerSessionRegistry::PeerSessionRegistry( Teardown teardown ) :
        teardown_( std::move( teardown ) )
    {
    }

    PeerSessionRegistry::~PeerSessionRegistry()
    {
        while( true )
        {
            std::string peer;
            {
                const std::scoped_lock lock( mutex_ );
                if( active_peers_.empty() )
                {
                    break;
                }
                peer = active_peers_.begin()->first;
            }
            unexpected_disconnect( peer );
        }
    }

    SessionCredentials
    PeerSessionRegistry::open( std::string peer )
    {
        unexpected_disconnect( peer );

        const std::scoped_lock lock( mutex_ );
        const auto             sequence = next_session_++;
        SessionCredentials     credentials{
            .session = make_session_value( "session", sequence ),
            .token   = make_session_value( "token", sequence ),
        };
        sessions_.emplace(
            credentials.session,
            Session{ .token = credentials.token, .peer = peer, .resources = {} }
        );
        active_peers_.insert_or_assign( std::move( peer ), credentials.session );
        return credentials;
    }

    grpc::Status
    PeerSessionRegistry::adopt( std::string      peer,
                                std::string_view session,
                                std::string_view token )
    {
        const std::scoped_lock lock( mutex_ );
        const auto             entry = sessions_.find( std::string{ session } );
        if( entry == sessions_.end() || !token_matches( entry->second.token, token ) )
        {
            return unauthenticated( "session adoption rejected: invalid credentials" );
        }

        const auto existing = active_peers_.find( peer );
        if( existing != active_peers_.end() && existing->second != session )
        {
            return grpc::Status{
                grpc::StatusCode::FAILED_PRECONDITION,
                "peer already owns another session"
            };
        }

        const auto previous = active_peers_.find( entry->second.peer );
        if( previous != active_peers_.end() && previous->second == session )
        {
            active_peers_.erase( previous );
        }
        entry->second.peer = peer;
        active_peers_.insert_or_assign( std::move( peer ), std::string{ session } );
        return grpc::Status::OK;
    }

    grpc::Status
    PeerSessionRegistry::add_resource( std::string_view peer,
                                       std::string      resource )
    {
        const std::scoped_lock lock( mutex_ );
        const auto             active = active_peers_.find( std::string{ peer } );
        if( active == active_peers_.end() )
        {
            return grpc::Status{
                grpc::StatusCode::NOT_FOUND,
                "peer has no active session"
            };
        }
        auto session = sessions_.find( active->second );
        if( session == sessions_.end() )
        {
            return internal_error( "active peer session is missing" );
        }
        session->second.resources.insert_or_assign( std::move( resource ), Resource{} );
        return grpc::Status::OK;
    }

    grpc::Status
    PeerSessionRegistry::add_process( std::string_view   peer,
                                      std::string        resource,
                                      grab::OwnedProcess process )
    {
        const std::scoped_lock lock( mutex_ );
        const auto             active = active_peers_.find( std::string{ peer } );
        if( active == active_peers_.end() )
        {
            return grpc::Status{
                grpc::StatusCode::NOT_FOUND,
                "peer has no active session"
            };
        }
        auto session = sessions_.find( active->second );
        if( session == sessions_.end() )
        {
            return internal_error( "active peer session is missing" );
        }
        session->second.resources.insert_or_assign(
            std::move( resource ),
            Resource{ .process = std::move( process ) }
        );
        return grpc::Status::OK;
    }

    PeerSessionRegistry::CatalogScope
    PeerSessionRegistry::scope( std::string_view peer )
    {
        return CatalogScope{ *this, std::string{ peer }, catalog_snapshot( peer ) };
    }

    void
    PeerSessionRegistry::close( std::string_view peer,
                                PeerCloseReason /*reason*/ ) noexcept
    {
        std::vector<std::string> resource_names;
        std::vector<Resource>    resources;
        {
            const std::scoped_lock lock( mutex_ );
            const auto             active = active_peers_.find( std::string{ peer } );
            if( active == active_peers_.end() )
            {
                return;
            }
            const auto session = sessions_.find( active->second );
            if( session != sessions_.end() )
            {
                resource_names.reserve( session->second.resources.size() );
                resources.reserve( session->second.resources.size() );
                for( auto& [name, resource] : session->second.resources )
                {
                    resource_names.push_back( name );
                    resources.push_back( std::move( resource ) );
                }
                sessions_.erase( session );
            }
            active_peers_.erase( active );
            ++close_count_;
        }

        terminate_owned( resources );
        if( teardown_ )
        {
            teardown_( peer, resource_names );
        }
    }

    void
    PeerSessionRegistry::deliberate_close( std::string_view peer ) noexcept
    {
        close( peer, PeerCloseReason::Deliberate );
    }

    void
    PeerSessionRegistry::unexpected_disconnect( std::string_view peer ) noexcept
    {
        close( peer, PeerCloseReason::UnexpectedDisconnect );
    }

    bool
    PeerSessionRegistry::active( std::string_view peer ) const
    {
        const std::scoped_lock lock( mutex_ );
        return active_peers_.contains( std::string{ peer } );
    }

    std::size_t
    PeerSessionRegistry::close_count() const
    {
        const std::scoped_lock lock( mutex_ );
        return close_count_;
    }

    std::unordered_set<std::string>
    PeerSessionRegistry::catalog_snapshot( std::string_view peer ) const
    {
        std::unordered_set<std::string> snapshot;
        const std::scoped_lock          lock( mutex_ );
        const auto active = active_peers_.find( std::string{ peer } );
        if( active == active_peers_.end() )
        {
            return snapshot;
        }
        const auto session = sessions_.find( active->second );
        if( session == sessions_.end() )
        {
            return snapshot;
        }
        for( const auto& [name, resource] : session->second.resources )
        {
            static_cast<void>( resource );
            snapshot.insert( name );
        }
        return snapshot;
    }

    void
    PeerSessionRegistry::reap_diff(
        std::string_view                       peer,
        const std::unordered_set<std::string>& snapshot
    ) noexcept
    {
        std::vector<std::string> resource_names;
        std::vector<Resource>    resources;
        {
            const std::scoped_lock lock( mutex_ );
            const auto             active = active_peers_.find( std::string{ peer } );
            if( active == active_peers_.end() )
            {
                return;
            }
            const auto session = sessions_.find( active->second );
            if( session == sessions_.end() )
            {
                return;
            }
            auto& catalog = session->second.resources;
            for( auto entry = catalog.begin(); entry != catalog.end(); )
            {
                if( snapshot.contains( entry->first ) )
                {
                    ++entry;
                    continue;
                }
                resource_names.push_back( entry->first );
                resources.push_back( std::move( entry->second ) );
                entry = catalog.erase( entry );
            }
        }

        terminate_owned( resources );
        if( teardown_ && !resource_names.empty() )
        {
            teardown_( peer, resource_names );
        }
    }

    void
    PeerSessionRegistry::terminate_owned( std::vector<Resource>& resources ) noexcept
    {
        for( auto& resource : resources )
        {
            if( resource.process.has_value() )
            {
                static_cast<void>(
                    // Teardown is best-effort and cannot report failures from this
                    // noexcept path.
                    // NOLINTNEXTLINE(bugprone-unused-return-value)
                    resource.process->terminate( ownedProcessTerminationGrace )
                );
            }
        }
        resources.clear();
    }

    EventService::EventService( grab::EventBus&              bus,
                                const grab::ActiveKindProbe* probe,
                                ServiceOptions               options,
                                grab::Session*               session ) noexcept :
        bus_( &bus ),
        probe_( probe ),
        session_( session ),
        options_( options ),
        admission_( std::make_unique<AdmissionController>( options_.admission ) )
    {
    }

    EventService::~EventService() = default;

    grpc::Status
    EventService::PushEvent( grpc::ServerContext*                   context,
                             const eventgrab::v1::PushEventRequest* request,
                             eventgrab::v1::PushEventResponse* /*response*/ )
    {
        return dispatch( "PushEvent",
                         context,
                         [&]( grab::OperationContext& )
                         {
                             if( request == nullptr )
                             {
                                 return invalid_argument( "missing push request" );
                             }

                             auto event = grab::transport::from_wire( request->event() );
                             if( !event.has_value() )
                             {
                                 return invalid_argument( event.error().message );
                             }

                             bus_->publish( std::move( *event ) );
                             return grpc::Status::OK;
                         } );
    }

    grpc::Status
    EventService::ListEventTypes(
        grpc::ServerContext* context,
        const eventgrab::v1::ListEventTypesRequest* /*request*/,
        eventgrab::v1::ListEventTypesResponse* response
    )
    {
        return dispatch(
            "ListEventTypes",
            context,
            [&]( grab::OperationContext& )
            {
                if( response == nullptr )
                {
                    return internal_error( "missing list response" );
                }

                for( const auto& descriptor : grab::event_type_descriptors( probe_ ) )
                {
                    auto* type = response->add_types();
                    type->set_kind( grab::transport::to_wire_kind( descriptor.kind ) );
                    type->set_category(
                        grab::transport::to_wire_category( descriptor.category )
                    );
                    type->set_name( descriptor.name );
                    type->set_active( descriptor.active );
                }

                return grpc::Status::OK;
            }
        );
    }

    grpc::Status
    EventService::Subscribe( grpc::ServerContext*                      context,
                             const eventgrab::v1::EventFilter*         request,
                             grpc::ServerWriter<eventgrab::v1::Event>* writer )
    {
        return dispatch(
            "Subscribe",
            context,
            [&]( grab::OperationContext& )
            {
                if( context == nullptr || request == nullptr || writer == nullptr )
                {
                    return invalid_argument( "missing subscribe request" );
                }

                auto filter = from_wire_filter( *request );
                if( !filter.has_value() )
                {
                    return invalid_argument( filter.error().message );
                }

                auto subscription = bus_->subscribe( std::move( *filter ) );
                auto notify_state = std::make_shared<NotifyState>();
                subscription.set_notify(
                    [notify_state]
                    {
                        notify_waiter( notify_state );
                    }
                );

                // The synchronous gRPC API runs one server thread per subscriber and
                // permits exactly one blocking write here. Async CQ streaming is a
                // future scale optimization, not needed for this correctness path.
                writer->SendInitialMetadata();
                while( !context->IsCancelled() )
                {
                    auto status =
                        write_available_events( *context, *writer, subscription );
                    if( !status.ok() )
                    {
                        subscription.set_notify( {} );
                        notify_waiter( notify_state );
                        return status;
                    }

                    wait_for_data_or_poll_interval( notify_state,
                                                    options_.poll_interval );
                }

                subscription.set_notify( {} );
                notify_waiter( notify_state );
                return grpc::Status::OK;
            }
        );
    }

    grpc::Status
    EventService::SetClientContext(
        grpc::ServerContext*                          context,
        const eventgrab::v1::SetClientContextRequest* request,
        eventgrab::v1::SetClientContextResponse*      response
    )
    {
        return dispatch(
            "SetClientContext",
            context,
            [&]( grab::OperationContext& )
            {
                if( context == nullptr || request == nullptr || response == nullptr )
                {
                    return invalid_argument( "missing client context request" );
                }

                const auto sequence = request->sequence();
                {
                    const std::scoped_lock lock( client_context_mutex_ );
                    auto& previous = client_context_sequence_[request->context()];
                    if( sequence <= previous )
                    {
                        return invalid_argument(
                            "client context sequence must increase monotonically"
                        );
                    }
                    previous = sequence;
                }
                response->set_sequence( sequence );
                auto* diagnostic = response->mutable_diagnostics()->add_log();
                diagnostic->set_sequence( sequence );
                diagnostic->set_message( std::string{ "client context accepted: " } +
                                         request->context() );
                return grpc::Status::OK;
            }
        );
    }

    grpc::Status
    EventService::ResolveNode( grpc::ServerContext*                     context,
                               const eventgrab::v1::ResolveNodeRequest* request,
                               eventgrab::v1::ResolveNodeResponse*      response )
    {
        return dispatch( "ResolveNode",
                         context,
                         [&]( grab::OperationContext& )
                         {
                             if( request == nullptr || response == nullptr )
                             {
                                 return invalid_argument(
                                     "missing resolve node request or response"
                                 );
                             }
                             if( session_ == nullptr )
                             {
                                 return status_from_error( capability_error(
                                     "daemon has no session for node resolution"
                                 ) );
                             }

                             auto locator =
                                 grab::Locator::from_string( request->locator() );
                             if( !locator.has_value() )
                             {
                                 return invalid_argument( locator.error().message );
                             }

                             const auto cardinality =
                                 decode_cardinality( request->cardinality() );
                             if( !cardinality.has_value() )
                             {
                                 return invalid_argument( "invalid cardinality" );
                             }

                             auto match = session_->resolve( *locator, *cardinality );
                             if( !match.has_value() )
                             {
                                 return status_from_error( match.error() );
                             }

                             encode_match( *match, response->mutable_match() );
                             return grpc::Status::OK;
                         } );
    }

    grpc::Status
    EventService::PerformAction( grpc::ServerContext*                       context,
                                 const eventgrab::v1::PerformActionRequest* request,
                                 eventgrab::v1::PerformActionResponse*      response )
    {
        return dispatch(
            "PerformAction",
            context,
            [&]( grab::OperationContext& )
            {
                if( request == nullptr || response == nullptr )
                {
                    return invalid_argument(
                        "missing perform action request or response"
                    );
                }

                const std::string_view command{ request->command() };
                const auto&            commands = grab::list_commands();
                const auto             descriptor =
                    std::ranges::find( commands,
                                       command,
                                       &grab::CommandDescriptor::name );
                if( descriptor == commands.end() )
                {
                    std::string message{ "unknown command '" };
                    message.append( command );
                    message.append( "'; known commands: " );
                    message.append( known_command_names() );
                    return invalid_argument( message );
                }

                if( descriptor->kind !=
                    grab::CommandKind::Click &&
                    descriptor->kind != grab::CommandKind::Type )
                {
                    return invalid_argument(
                        "command is not performable over PerformAction"
                    );
                }
                if( session_ == nullptr )
                {
                    return status_from_error(
                        capability_error( "daemon has no session for actions" )
                    );
                }

                // No consent-grant machinery exists yet (Wave-2 scope); mutating
                // consent-gated commands are refused at the wire boundary. Read-only
                // consent-gated commands (screen.capture) ride the implicit observation
                // grant.
                if( descriptor->mutability ==
                    grab::Mutability::Mutating &&
                    descriptor->consent_gated )
                {
                    std::string message{ "command '" };
                    message.append( descriptor->name );
                    message.append(
                        "' is consent-gated and no grant machinery exists yet"
                    );
                    return status_from_error( grab::Error{
                        .code        = grab::ErrorCode::PermissionNeeded,
                        .message     = std::move( message ),
                        .capability  = {},
                        .target      = {},
                        .attempts    = {},
                        .disposition = grab::ErrorDisposition::Fatal,
                        .diagnostics = {},
                    } );
                }

                std::optional<grab::ActionTarget> target;
                if( request->has_target_ref() )
                {
                    target.emplace( grab::Match{
                        .ref                = decode_widget_ref( request->target_ref() ),
                        .mode               = grab::ConsistencyMode::Live,
                        .snapshot_revision  = 0U,
                        .matched_predicates = {},
                        .provenance         = {},
                    } );
                }
                else
                {
                    auto locator = grab::Locator::from_string( request->locator() );
                    if( !locator.has_value() )
                    {
                        return invalid_argument( locator.error().message );
                    }
                    target.emplace( std::move( *locator ) );
                }

                grab::ActionOptions options{};
                const auto&         wire_options = request->options();
                if( wire_options.deadline_ms() > 0U )
                {
                    options.deadline = std::chrono::milliseconds{
                        static_cast<std::chrono::milliseconds::rep>(
                            wire_options.deadline_ms()
                        )
                    };
                }

                const auto cardinality =
                    decode_cardinality( wire_options.cardinality() );
                if( !cardinality.has_value() )
                {
                    return invalid_argument( "invalid cardinality" );
                }
                options.cardinality = *cardinality;

                const auto routing  = decode_routing( wire_options.routing() );
                if( !routing.has_value() )
                {
                    return invalid_argument( "invalid routing policy" );
                }
                options.routing  = *routing;

                const auto retry = decode_retry( wire_options.retry() );
                if( !retry.has_value() )
                {
                    return invalid_argument( "invalid retry class" );
                }
                options.retry = *retry;
                options.force = wire_options.force();

                std::optional<grab::Action> action;
                if( descriptor->kind == grab::CommandKind::Click )
                {
                    action.emplace( grab::Click{ .target = std::move( *target ) } );
                }
                else
                {
                    action.emplace( grab::TypeText{
                        .target = std::move( *target ),
                        .text   = request->text(),
                    } );
                }

                auto receipt = session_->perform( *action, options );
                if( !receipt.has_value() )
                {
                    return status_from_error( receipt.error() );
                }

                auto* wire = response->mutable_receipt();
                wire->set_commit_status( std::string{
                    grab::detail::commit_status_name.text_of( receipt->commit, "" )
                } );
                wire->set_fallback_used( receipt->fallback_used );
                wire->set_forced( receipt->forced );
                for( const auto& attempt : receipt->routes )
                {
                    wire->add_routes( attempt.route );
                }
                wire->set_locator( receipt->locator );
                wire->set_snapshot_revision( receipt->snapshot_revision );
                return grpc::Status::OK;
            }
        );
    }

    grpc::Status
    EventService::CaptureFrame( grpc::ServerContext*                      context,
                                const eventgrab::v1::CaptureFrameRequest* request,
                                eventgrab::v1::CaptureFrameResponse*      response )
    {
        return dispatch(
            "CaptureFrame",
            context,
            [&]( grab::OperationContext& )
            {
                if( request == nullptr || response == nullptr )
                {
                    return invalid_argument(
                        "missing capture frame request or response"
                    );
                }

                constexpr std::string_view captureCommand{ "screen.capture" };
                const auto&                commands = grab::list_commands();
                const auto                 descriptor =
                    std::ranges::find( commands,
                                       captureCommand,
                                       &grab::CommandDescriptor::name );
                if( descriptor == commands.end() )
                {
                    return internal_error(
                        "screen.capture command descriptor is missing"
                    );
                }
                // Read-only consent-gated commands (screen.capture) ride the implicit
                // observation grant.

                const bool has_output  = !request->output().empty();
                const bool has_locator = !request->locator().empty();
                if( has_output == has_locator )
                {
                    return invalid_argument(
                        "capture requires exactly one of output or locator"
                    );
                }
                if( session_ == nullptr )
                {
                    return status_from_error(
                        capability_error( "daemon has no session for capture" )
                    );
                }

                std::optional<grab::CaptureTarget> target;
                if( has_locator )
                {
                    auto locator = grab::Locator::from_string( request->locator() );
                    if( !locator.has_value() )
                    {
                        return invalid_argument( locator.error().message );
                    }
                    auto match =
                        session_->resolve( *locator, grab::Cardinality::ExactlyOne );
                    if( !match.has_value() )
                    {
                        return status_from_error( match.error() );
                    }
                    target.emplace( std::move( *match ) );
                }
                else
                {
                    target.emplace( request->output() );
                }

                auto frame = session_->capture( *target );
                if( !frame.has_value() )
                {
                    return status_from_error( frame.error() );
                }

                auto png = grab::codec::encode_png( frame->image );
                if( !png.has_value() )
                {
                    return internal_error( png.error().message );
                }
                response->set_png( std::string{
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                    reinterpret_cast<const char*>( png->data() ),
                    png->size()
                } );

                auto* meta = response->mutable_meta();
                meta->set_frame_id( frame->id.value );
                meta->set_space( frame->space.value );
                meta->set_generation( frame->generation.value );
                meta->set_captured_at_ns( frame->captured_at_ns );
                return grpc::Status::OK;
            }
        );
    }

    grpc::Status
    EventService::dispatch(
        std::string_view                                              rpc_name,
        grpc::ServerContext*                                          context,
        const std::function<grpc::Status( grab::OperationContext& )>& work
    )
    {
        {
            const std::scoped_lock lock( admission_log_mutex_ );
            ++admission_log_[std::string{ rpc_name }];
        }

        if( wrapped_rpc_count( rpc_name ) != 1U )
        {
            return internal_error( "RPC is not registered with admission control" );
        }

        grab::OperationContext operation;
        return admission_->run( context,
                                [&]
                                {
                                    return work( operation );
                                } );
    }

    std::size_t
    EventService::registered_rpc_count() const noexcept
    {
        return registeredRpcNames.size();
    }

    std::size_t
    EventService::wrapped_rpc_count( std::string_view rpc_name ) const noexcept
    {
        return static_cast<std::size_t>(
            std::count( registeredRpcNames.begin(), registeredRpcNames.end(), rpc_name )
        );
    }

    std::size_t
    EventService::admission_entry_count( std::string_view rpc_name ) const noexcept
    {
        const std::scoped_lock lock( admission_log_mutex_ );
        for( const auto& [name, count] : admission_log_ )
        {
            if( name == rpc_name )
            {
                return count;
            }
        }
        return 0U;
    }

}    // namespace grab::transport
