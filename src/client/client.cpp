#include "client/client.hpp"
#include "client/transport.hpp"
#include "grab/capture.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/event_descriptor.hpp"
#include "grab/interaction.hpp"
#include "grab/locator.hpp"
#include "grab/process_ref.hpp"
#include "grab/query.hpp"
#include "grab/result.hpp"
#include "grab/trace.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <expected>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/poll.h>
#include <system_error>
#include <utility>
#include <vector>

namespace grab::client
{
    namespace
    {

        [[nodiscard]]
        std::filesystem::path
        socket_path_from_endpoint( std::string_view endpoint )
        {
            constexpr std::string_view unixTripleSlash{ "unix://" };
            constexpr std::string_view unixPrefix{ "unix:" };
            if( endpoint.starts_with( unixTripleSlash ) )
            {
                return std::filesystem::path{
                    endpoint.substr( unixTripleSlash.size() )
                };
            }
            if( endpoint.starts_with( unixPrefix ) )
            {
                return std::filesystem::path{ endpoint.substr( unixPrefix.size() ) };
            }
            return {};
        }

        [[nodiscard]]
        grab::Error
        connection_failure( std::string message )
        {
            return grab::Error{
                .code        = grab::ErrorCode::EnvironmentChanged,
                .message     = std::move( message ),
                .capability  = {},
                .target      = {},
                .attempts    = {},
                .disposition = grab::ErrorDisposition::RetrySame,
            };
        }

        void
        wait_for_process_exit_or_timeout( int                                 pidfd,
                                          std::chrono::milliseconds           backoff,
                                          std::chrono::steady_clock::duration remaining )
        {
            const auto timeout = std::min(
                backoff,
                std::chrono::duration_cast<std::chrono::milliseconds>( remaining )
            );
            const auto timeout_milliseconds = static_cast<int>(
                std::min( timeout,
                          std::chrono::milliseconds{ std::numeric_limits<int>::max() } )
                    .count()
            );
            pollfd process_status{
                .fd      = pidfd,
                .events  = POLLIN,
                .revents = 0,
            };
            ( void )::poll( &process_status, 1, timeout_milliseconds );
        }

    }    // namespace

    bool
    is_connection_error( const grab::Error& error ) noexcept
    {
        return error.disposition == grab::ErrorDisposition::RetrySame;
    }

    class Client::State final : public std::enable_shared_from_this<Client::State>
    {
        private:

            struct Registration
            {
                    grab::EventFilter  filter;
                    SubscriptionHandle stream;
            };

            class ReplayableStream final : public SubscriptionStream
            {
                public:

                    ReplayableStream(
                        std::weak_ptr<State>          owner,
                        std::shared_ptr<Registration> registration
                    ) noexcept :
                        owner_( std::move( owner ) ),
                        registration_( std::move( registration ) )
                    {
                    }

                    [[nodiscard]]
                    grab::Result<std::optional<grab::SubscriptionEvent>>
                    try_next() override
                    {
                        if( registration_->stream == nullptr )
                        {
                            return grab::fail( grab::ErrorCode::SubscriptionGone,
                                               "subscription is no longer registered" );
                        }

                        auto next = registration_->stream->try_next();
                        if( next.has_value() || !is_connection_error( next.error() ) )
                        {
                            return next;
                        }

                        auto owner = owner_.lock();
                        if( owner == nullptr )
                        {
                            return grab::fail( grab::ErrorCode::SubscriptionGone,
                                               "subscription client no longer exists" );
                        }
                        auto recovered = owner->recover_connection();
                        if( !recovered.has_value() )
                        {
                            return std::unexpected( std::move( recovered.error() ) );
                        }
                        if( registration_->stream == nullptr )
                        {
                            return grab::fail( grab::ErrorCode::SubscriptionGone,
                                               "subscription replay failed" );
                        }
                        return registration_->stream->try_next();
                    }

                private:

                    std::weak_ptr<State>          owner_;
                    std::shared_ptr<Registration> registration_;
            };

        public:

            explicit State( Transport& transport ) noexcept :
                transport_( &transport )
            {
            }

            explicit State( std::unique_ptr<Transport> transport ) noexcept :
                owned_transport_( std::move( transport ) ),
                transport_( owned_transport_.get() )
            {
            }

            State( Transport&    transport,
                   DaemonOptions options ) :
                transport_( &transport ),
                daemon_options_( std::move( options ) )
            {
            }

            State( std::unique_ptr<Transport> transport,
                   DaemonOptions              options ) :
                owned_transport_( std::move( transport ) ),
                transport_( owned_transport_.get() ),
                daemon_options_( std::move( options ) )
            {
            }

            ~State()
            {
                terminate_daemon();
            }

            State( const State& ) = delete;
            State&
            operator=( const State& ) = delete;
            State( State&& )          = delete;
            State&
            operator=( State&& ) = delete;

            [[nodiscard]]
            grab::Result<void>
            ensure_daemon()
            {
                return ensure_daemon_impl( false );
            }

            [[nodiscard]]
            grab::Result<void>
            ensure_daemon( DaemonOptions options )
            {
                daemon_options_ = std::move( options );
                return ensure_daemon_impl( false );
            }

            [[nodiscard]]
            grab::Result<grab::Match>
            resolve( const grab::Locator& locator,
                     grab::Cardinality    cardinality )
            {
                auto bound = transport();
                if( !bound.has_value() )
                {
                    return std::unexpected( bound.error() );
                }
                auto resolved = ( *bound )->resolve( locator, cardinality );
                if( resolved.has_value() || !is_connection_error( resolved.error() ) )
                {
                    return resolved;
                }
                auto recovered = recover_connection();
                if( !recovered.has_value() )
                {
                    return std::unexpected( std::move( recovered.error() ) );
                }
                return ( *bound )->resolve( locator, cardinality );
            }

            [[nodiscard]]
            grab::Result<grab::Receipt>
            perform( const grab::Action&        action,
                     const grab::ActionOptions& options )
            {
                auto bound = transport();
                if( !bound.has_value() )
                {
                    return std::unexpected( bound.error() );
                }
                auto performed = ( *bound )->perform( action, options );
                if( performed.has_value() || !is_connection_error( performed.error() ) )
                {
                    return performed;
                }
                auto recovered = recover_connection();
                if( !recovered.has_value() )
                {
                    return std::unexpected( std::move( recovered.error() ) );
                }
                return ( *bound )->perform( action, options );
            }

            [[nodiscard]]
            grab::Result<grab::Frame>
            capture( const grab::CaptureTarget&  target,
                     const grab::CaptureOptions& options )
            {
                auto bound = transport();
                if( !bound.has_value() )
                {
                    return std::unexpected( bound.error() );
                }
                auto captured = ( *bound )->capture( target, options );
                if( captured.has_value() || !is_connection_error( captured.error() ) )
                {
                    return captured;
                }
                auto recovered = recover_connection();
                if( !recovered.has_value() )
                {
                    return std::unexpected( std::move( recovered.error() ) );
                }
                return ( *bound )->capture( target, options );
            }

            [[nodiscard]]
            grab::Result<void>
            push_event( const grab::Event& event )
            {
                auto bound = transport();
                if( !bound.has_value() )
                {
                    return std::unexpected( bound.error() );
                }
                auto pushed = ( *bound )->push_event( event );
                if( pushed.has_value() || !is_connection_error( pushed.error() ) )
                {
                    return pushed;
                }
                auto recovered = recover_connection();
                if( !recovered.has_value() )
                {
                    return recovered;
                }
                return ( *bound )->push_event( event );
            }

            [[nodiscard]]
            grab::Result<SubscriptionHandle>
            subscribe( grab::EventFilter filter )
            {
                auto bound = transport();
                if( !bound.has_value() )
                {
                    return std::unexpected( bound.error() );
                }

                auto stream = ( *bound )->subscribe( filter );
                if( !stream.has_value() && is_connection_error( stream.error() ) )
                {
                    auto recovered = recover_connection();
                    if( !recovered.has_value() )
                    {
                        return std::unexpected( std::move( recovered.error() ) );
                    }
                    stream = ( *bound )->subscribe( filter );
                }
                if( !stream.has_value() )
                {
                    return std::unexpected( std::move( stream.error() ) );
                }

                auto registration = std::make_shared<Registration>( Registration{
                    .filter = std::move( filter ),
                    .stream = std::move( *stream ),
                } );
                registrations_.push_back( registration );
                SubscriptionHandle replayable =
                    std::make_unique<ReplayableStream>( weak_from_this(),
                                                        std::move( registration ) );
                return replayable;
            }

            [[nodiscard]]
            grab::Result<std::vector<grab::EventTypeDescriptor>>
            list_event_types()
            {
                auto bound = transport();
                if( !bound.has_value() )
                {
                    return std::unexpected( bound.error() );
                }
                auto listed = ( *bound )->list_event_types();
                if( listed.has_value() || !is_connection_error( listed.error() ) )
                {
                    return listed;
                }
                auto recovered = recover_connection();
                if( !recovered.has_value() )
                {
                    return std::unexpected( std::move( recovered.error() ) );
                }
                return ( *bound )->list_event_types();
            }

        private:

            [[nodiscard]]
            grab::Result<Transport*>
            transport() noexcept
            {
                if( transport_ == nullptr )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "client transport must not be null" );
                }
                return transport_;
            }

            void
            terminate_daemon() noexcept
            {
                if( !daemon_.has_value() )
                {
                    return;
                }
                const auto grace = daemon_options_.has_value()
                                     ? daemon_options_->termination_grace
                                     : std::chrono::milliseconds{ 250 };
                if( daemon_->alive() )
                {
                    [[maybe_unused]]
                    auto terminated = daemon_->terminate( grace );
                }
                daemon_.reset();
            }

            [[nodiscard]]
            grab::Result<void>
            ensure_daemon_impl( bool restarting )
            {
                auto bound = transport();
                if( !bound.has_value() )
                {
                    return std::unexpected( bound.error() );
                }
                if( !daemon_options_.has_value() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "daemon lifecycle is not configured" );
                }

                DaemonOptions& options = *daemon_options_;
                if( options.endpoint.empty() && !options.socket_path.empty() )
                {
                    options.endpoint = "unix:" + options.socket_path.string();
                }
                if( options.socket_path.empty() )
                {
                    options.socket_path = socket_path_from_endpoint( options.endpoint );
                }
                if( options.endpoint.empty() ||
                    options.socket_path.empty() ||
                    options.executable.empty() )
                {
                    return grab::fail(
                        grab::ErrorCode::InvalidArgument,
                        "daemon endpoint, socket path, and executable are required"
                    );
                }

                if( restarting )
                {
                    terminate_daemon();
                    std::error_code remove_error;
                    static_cast<void>( std::filesystem::remove( options.socket_path,
                                                                remove_error ) );
                    if( remove_error )
                    {
                        return grab::fail( grab::ErrorCode::ProviderFailed,
                                           "failed to remove daemon socket: " +
                                               remove_error.message() );
                    }
                }

                std::error_code exists_error;
                const bool      socket_exists =
                    std::filesystem::exists( options.socket_path, exists_error );
                if( exists_error )
                {
                    return grab::fail( grab::ErrorCode::ProviderFailed,
                                       "failed to inspect daemon socket: " +
                                           exists_error.message() );
                }
                if( socket_exists )
                {
                    auto health = ( *bound )->list_event_types();
                    if( health.has_value() )
                    {
                        return {};
                    }
                    return std::unexpected( std::move( health.error() ) );
                }

                const std::array<std::string_view, 4U> arguments{
                    options.executable,
                    "daemon",
                    "--endpoint",
                    options.endpoint,
                };
                auto spawned = grab::OwnedProcess::spawn( arguments );
                if( !spawned.has_value() )
                {
                    return std::unexpected( std::move( spawned.error() ) );
                }
                daemon_             = std::move( *spawned );

                const auto timeout  = std::max( options.startup_timeout,
                                                std::chrono::milliseconds::zero() );
                const auto deadline = std::chrono::steady_clock::now() + timeout;
                auto       backoff =
                    std::max( options.initial_backoff, std::chrono::milliseconds{ 1 } );
                const auto maximum_backoff =
                    std::max( options.maximum_backoff, backoff );
                grab::Error last_error = connection_failure( "daemon is not ready" );

                while( true )
                {
                    auto health = ( *bound )->list_event_types();
                    if( health.has_value() )
                    {
                        return {};
                    }
                    last_error = std::move( health.error() );
                    if( !is_connection_error( last_error ) )
                    {
                        return std::unexpected( std::move( last_error ) );
                    }
                    if( !daemon_->alive() )
                    {
                        return std::unexpected( connection_failure(
                            "spawned daemon exited before becoming ready"
                        ) );
                    }

                    const auto now = std::chrono::steady_clock::now();
                    if( now >= deadline )
                    {
                        last_error.message =
                            "daemon readiness timed out: " + last_error.message;
                        return std::unexpected( std::move( last_error ) );
                    }
                    wait_for_process_exit_or_timeout( daemon_->pidfd(),
                                                      backoff,
                                                      deadline - now );
                    backoff = std::min( maximum_backoff, backoff * 2 );
                }
            }

            [[nodiscard]]
            grab::Result<void>
            recover_connection()
            {
                auto restarted = ensure_daemon_impl( true );
                if( !restarted.has_value() )
                {
                    return restarted;
                }

                auto bound = transport();
                if( !bound.has_value() )
                {
                    return std::unexpected( bound.error() );
                }
                std::erase_if( registrations_,
                               []( const auto& registration )
                               {
                                   return registration.expired();
                               } );
                for( auto& weak_registration : registrations_ )
                {
                    auto registration = weak_registration.lock();
                    auto replayed     = ( *bound )->subscribe( registration->filter );
                    if( !replayed.has_value() )
                    {
                        return std::unexpected( std::move( replayed.error() ) );
                    }
                    registration->stream = std::move( *replayed );
                }
                return {};
            }

            std::unique_ptr<Transport>               owned_transport_;
            Transport*                               transport_ = nullptr;
            std::optional<DaemonOptions>             daemon_options_;
            std::optional<grab::OwnedProcess>        daemon_;
            std::vector<std::weak_ptr<Registration>> registrations_;
    };

    Client::Client( Transport& transport ) noexcept :
        state_( std::make_shared<State>( transport ) )
    {
    }

    Client::Client( std::unique_ptr<Transport> transport ) noexcept :
        state_( std::make_shared<State>( std::move( transport ) ) )
    {
    }

    Client::Client( Transport&    transport,
                    DaemonOptions options ) :
        state_( std::make_shared<State>( transport,
                                         std::move( options ) ) )
    {
    }

    Client::Client( std::unique_ptr<Transport> transport,
                    DaemonOptions              options ) :
        state_( std::make_shared<State>( std::move( transport ),
                                         std::move( options ) ) )
    {
    }

    Client::~Client()                   = default;
    Client::Client( Client&& ) noexcept = default;
    Client&
    Client::operator=( Client&& ) noexcept = default;

    grab::Result<void>
    Client::ensure_daemon()
    {
        return state_->ensure_daemon();
    }

    grab::Result<void>
    Client::ensure_daemon( DaemonOptions options )
    {
        return state_->ensure_daemon( std::move( options ) );
    }

    grab::Result<grab::Match>
    Client::resolve( const grab::Locator& locator,
                     grab::Cardinality    cardinality )
    {
        return state_->resolve( locator, cardinality );
    }

    grab::Result<grab::Receipt>
    Client::perform( const grab::Action&        action,
                     const grab::ActionOptions& options )
    {
        return state_->perform( action, options );
    }

    grab::Result<grab::Frame>
    Client::capture( const grab::CaptureTarget&  target,
                     const grab::CaptureOptions& options )
    {
        return state_->capture( target, options );
    }

    grab::Result<void>
    Client::push_event( grab::Event event )
    {
        return state_->push_event( event );
    }

    grab::Result<SubscriptionHandle>
    Client::subscribe( grab::EventFilter filter )
    {
        return state_->subscribe( std::move( filter ) );
    }

    grab::Result<std::vector<grab::EventTypeDescriptor>>
    Client::list_event_types()
    {
        return state_->list_event_types();
    }

}    // namespace grab::client
