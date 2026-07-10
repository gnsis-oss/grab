#include "core/reactor.hpp"
#include "event/platform_factory.hpp"
#include "event/source_registry.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/result.hpp"
#include "service/daemon.hpp"
#include "storage/jsonl_sink.hpp"
#include "transport/server.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <expected>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace grab::service
{
    namespace
    {

        constexpr std::size_t      storageQueueDepth  = 65'536U;
        constexpr std::size_t      storageBufferLimit = 1U;
        constexpr std::string_view reactorThreadStartStep =
            "daemon reactor thread start";
        constexpr std::string_view reactorRunStep = "daemon reactor run";

        struct DrainState
        {
                std::mutex              mutex;
                std::condition_variable data_ready;
                bool                    notified = false;
                bool                    stopping = false;
        };

        [[nodiscard]]
        grab::Error
        make_internal_error( const std::string& message )
        {
            return grab::Error{
                .code       = grab::ErrorCode::InternalFault,
                .message    = message,
                .capability = {},
                .target     = {},
                .attempts   = {},
            };
        }

        [[nodiscard]]
        std::unexpected<grab::Error>
        internal_failure( const std::string& message )
        {
            return std::unexpected( make_internal_error( message ) );
        }

        [[nodiscard]]
        std::unexpected<grab::Error>
        exception_failure( std::string_view      step,
                           const std::exception& exception )
        {
            return internal_failure( std::string{ step } + ": " + exception.what() );
        }

        [[nodiscard]]
        std::unexpected<grab::Error>
        unknown_exception_failure( std::string_view step )
        {
            return internal_failure( std::string{ step } + ": unknown exception" );
        }

        void
        notify_waiter( const std::shared_ptr<DrainState>& state )
        {
            {
                const std::scoped_lock lock( state->mutex );
                state->notified = true;
            }
            state->data_ready.notify_one();
        }

        [[nodiscard]]
        bool
        wait_for_data_or_stop( const std::shared_ptr<DrainState>& state )
        {
            std::unique_lock lock( state->mutex );
            state->data_ready.wait( lock,
                                    [&]
                                    {
                                        return state->notified || state->stopping;
                                    } );
            state->notified = false;
            return state->stopping;
        }

        void
        request_stop( const std::shared_ptr<DrainState>& state ) noexcept
        {
            try
            {
                {
                    const std::scoped_lock lock( state->mutex );
                    state->stopping = true;
                    state->notified = true;
                }
                state->data_ready.notify_one();
            }
            catch( ... )
            {
                return;
            }
        }

        class StartupSignal
        {
            public:

                StartupSignal()                       = default;
                ~StartupSignal()                      = default;

                StartupSignal( const StartupSignal& ) = delete;
                StartupSignal&
                operator=( const StartupSignal& ) = delete;
                StartupSignal( StartupSignal&& )  = delete;
                StartupSignal&
                operator=( StartupSignal&& ) = delete;

                [[nodiscard]]
                std::future<grab::Result<void>>
                future()
                {
                    return result_.get_future();
                }

                void
                report( grab::Result<void> result )
                {
                    const std::scoped_lock lock( mutex_ );
                    if( reported_ )
                    {
                        return;
                    }
                    reported_ = true;
                    result_.set_value( std::move( result ) );
                }

            private:

                std::mutex                       mutex_;
                bool                             reported_ = false;
                std::promise<grab::Result<void>> result_;
        };

    }    // namespace

    class Daemon::Impl
    {
        public:

            explicit Impl( DaemonOptions options );
            ~Impl() noexcept;

            Impl( const Impl& ) = delete;
            Impl&
            operator=( const Impl& ) = delete;
            Impl( Impl&& )           = delete;
            Impl&
            operator=( Impl&& ) = delete;

            [[nodiscard]]
            grab::Result<void>
            start_storage();

            [[nodiscard]]
            grab::Result<void>
            start_transport();

            [[nodiscard]]
            grab::Result<void>
            start_sources();

            void
            shutdown() noexcept;

            [[nodiscard]]
            grab::EventBus&
            bus() noexcept;

            [[nodiscard]]
            const std::string&
            endpoint() const noexcept;

        private:

            [[nodiscard]]
            grab::Result<void>
            start_drain_thread();

            [[nodiscard]]
            grab::Result<void>
            run_reactor();

            void
            drain_loop( std::promise<grab::Result<void>> ready ) noexcept;

            void
            drain_available( grab::Subscription& subscription ) noexcept;

            void
            signal_drain_stop() noexcept;

            void
            join_drain() noexcept;

            void
            join_reactor() noexcept;

            void
                                                 flush_and_close_sink() noexcept;

            std::string                          endpoint_;
            std::optional<std::filesystem::path> store_dir_;
            std::function<std::vector<std::unique_ptr<grab::event::EventSource>>()>
                                                            source_factory_;
            grab::EventBus                                  bus_;
            grab::core::Reactor                             reactor_;
            grab::event::SourceRegistry                     registry_;
            std::optional<grab::transport::TransportServer> server_;
            std::optional<grab::storage::JsonlSink>         sink_;
            std::shared_ptr<DrainState>                     drain_state_;
            std::thread                                     reactor_thread_;
            std::thread                                     drain_thread_;
            std::atomic_bool                                shutdown_started_{ false };
    };

    Daemon::Impl::Impl( DaemonOptions options ) :
        endpoint_( std::move( options.endpoint ) ),
        store_dir_( std::move( options.store_dir ) ),
        source_factory_( std::move( options.source_factory ) ),
        drain_state_( std::make_shared<DrainState>() )
    {
    }

    Daemon::Impl::~Impl() noexcept
    {
        shutdown();
        // shutdown() is idempotent and no-ops if it already ran (e.g. it was
        // triggered from the reactor thread, where join_reactor() could not
        // self-join). Join unconditionally here, on the owner thread, so a
        // still-joinable reactor thread never reaches std::thread's terminating
        // destructor.
        join_reactor();
    }

    grab::Result<void>
    Daemon::Impl::start_storage()
    {
        if( !store_dir_.has_value() )
        {
            return {};
        }

        auto sink = grab::storage::JsonlSink::open( grab::storage::JsonlOptions{
            .dir          = *store_dir_,
            .buffer_limit = storageBufferLimit,
        } );
        if( !sink.has_value() )
        {
            return std::unexpected( std::move( sink.error() ) );
        }

        sink_.emplace( std::move( *sink ) );
        return start_drain_thread();
    }

    grab::Result<void>
    Daemon::Impl::start_transport()
    {
        auto transport =
            grab::transport::TransportServer::start( endpoint_, bus_, &registry_ );
        if( !transport.has_value() )
        {
            return std::unexpected( std::move( transport.error() ) );
        }

        server_.emplace( std::move( *transport ) );
        return {};
    }

    grab::Result<void>
    Daemon::Impl::start_sources()
    {
        const auto startup = std::make_shared<StartupSignal>();
        auto       ready   = startup->future();

        try
        {
            reactor_.post(
                [startup]
                {
                    startup->report( grab::Result<void>{} );
                }
            );

            reactor_thread_ = std::thread(
                [this, startup]
                {
                    startup->report( run_reactor() );
                }
            );
        }
        catch( const std::exception& exception )
        {
            reactor_.stop();
            join_reactor();
            return exception_failure( reactorThreadStartStep, exception );
        }
        catch( ... )
        {
            reactor_.stop();
            join_reactor();
            return unknown_exception_failure( reactorThreadStartStep );
        }

        auto ready_result = ready.get();
        if( !ready_result.has_value() )
        {
            auto error = std::move( ready_result.error() );
            reactor_.stop();
            join_reactor();
            return std::unexpected( std::move( error ) );
        }

        auto sources =
            source_factory_
                ? source_factory_()
                : grab::event::PlatformFactory::build( grab::event::SourceConfig{} );
        for( auto& source : sources )
        {
            registry_.add( std::move( source ) );
        }

        auto start_result = registry_.start_all( reactor_, bus_ );
        static_cast<void>( start_result );
        return {};
    }

    grab::Result<void>
    Daemon::Impl::start_drain_thread()
    {
        std::promise<grab::Result<void>> ready_promise;
        auto                             ready = ready_promise.get_future();

        try
        {
            drain_thread_ = std::thread(
                [this, ready_promise = std::move( ready_promise )]() mutable
                {
                    drain_loop( std::move( ready_promise ) );
                }
            );
        }
        catch( const std::exception& exception )
        {
            return exception_failure( "daemon storage thread start", exception );
        }
        catch( ... )
        {
            return unknown_exception_failure( "daemon storage thread start" );
        }

        auto ready_result = ready.get();
        if( !ready_result.has_value() )
        {
            signal_drain_stop();
            join_drain();
            return std::unexpected( std::move( ready_result.error() ) );
        }
        return {};
    }

    grab::Result<void>
    Daemon::Impl::run_reactor()
    {
        try
        {
            return reactor_.run();
        }
        catch( const std::exception& exception )
        {
            return exception_failure( reactorRunStep, exception );
        }
        catch( ... )
        {
            return unknown_exception_failure( reactorRunStep );
        }
    }

    void
    Daemon::Impl::drain_loop( std::promise<grab::Result<void>> ready ) noexcept
    {
        bool ready_sent = false;
        try
        {
            // Storage currently rides a large-queue subscription (best-effort
            // under extreme overflow); a future EventBus durable-sink hook would
            // make it strictly lossless. This is a known follow-up.
            grab::EventFilter filter;
            auto subscription = bus_.subscribe( std::move( filter ), storageQueueDepth );
            subscription.set_notify(
                [state = drain_state_]
                {
                    notify_waiter( state );
                }
            );
            ready.set_value( {} );
            ready_sent = true;

            while( true )
            {
                drain_available( subscription );
                if( wait_for_data_or_stop( drain_state_ ) )
                {
                    drain_available( subscription );
                    break;
                }
            }

            subscription.set_notify( {} );
        }
        catch( const std::exception& exception )
        {
            if( !ready_sent )
            {
                ready.set_value( exception_failure( "daemon storage subscription",
                                                    exception ) );
            }
        }
        catch( ... )
        {
            if( !ready_sent )
            {
                ready.set_value(
                    unknown_exception_failure( "daemon storage subscription" )
                );
            }
        }
    }

    void
    Daemon::Impl::drain_available( grab::Subscription& subscription ) noexcept
    {
        while( true )
        {
            auto event = subscription.try_pop();
            if( !event.has_value() )
            {
                return;
            }

            if( !sink_.has_value() )
            {
                continue;
            }

            const auto write_result = sink_->write( *event );
            static_cast<void>( write_result );
        }
    }

    void
    Daemon::Impl::signal_drain_stop() noexcept
    {
        if( drain_state_ == nullptr )
        {
            return;
        }

        request_stop( drain_state_ );
    }

    void
    Daemon::Impl::join_drain() noexcept
    {
        try
        {
            if( drain_thread_.joinable() )
            {
                drain_thread_.join();
            }
        }
        catch( ... )
        {
            return;
        }
    }

    void
    Daemon::Impl::join_reactor() noexcept
    {
        try
        {
            if( reactor_thread_.joinable() &&
                reactor_thread_.get_id() != std::this_thread::get_id() )
            {
                reactor_thread_.join();
            }
        }
        catch( ... )
        {
            return;
        }
    }

    void
    Daemon::Impl::flush_and_close_sink() noexcept
    {
        if( !sink_.has_value() )
        {
            return;
        }

        try
        {
            const auto flush_result = sink_->flush();
            static_cast<void>( flush_result );
            sink_->close();
            sink_.reset();
        }
        catch( ... )
        {
            sink_.reset();
        }
    }

    void
    Daemon::Impl::shutdown() noexcept
    {
        if( shutdown_started_.exchange( true ) )
        {
            return;
        }

        registry_.stop_all();
        reactor_.stop();
        join_reactor();

        try
        {
            if( server_.has_value() )
            {
                server_->shutdown();
                server_.reset();
            }
        }
        catch( ... )
        {
            server_.reset();
        }

        signal_drain_stop();
        join_drain();
        flush_and_close_sink();
    }

    grab::EventBus&
    Daemon::Impl::bus() noexcept
    {
        return bus_;
    }

    const std::string&
    Daemon::Impl::endpoint() const noexcept
    {
        return endpoint_;
    }

    Daemon::Daemon( std::unique_ptr<Impl> impl ) noexcept :
        impl_( std::move( impl ) )
    {
    }

    Daemon::~Daemon()
    {
        shutdown();
    }

    Daemon::Daemon( Daemon&& other ) noexcept = default;

    Daemon&
    Daemon::operator=( Daemon&& other ) noexcept
    {
        if( this != &other )
        {
            shutdown();
            impl_ = std::move( other.impl_ );
        }
        return *this;
    }

    grab::Result<Daemon>
    Daemon::start( DaemonOptions options )
    {
        try
        {
            auto impl           = std::make_unique<Impl>( std::move( options ) );

            auto storage_result = impl->start_storage();
            if( !storage_result.has_value() )
            {
                impl->shutdown();
                return std::unexpected( std::move( storage_result.error() ) );
            }

            auto transport_result = impl->start_transport();
            if( !transport_result.has_value() )
            {
                impl->shutdown();
                return std::unexpected( std::move( transport_result.error() ) );
            }

            auto source_result = impl->start_sources();
            if( !source_result.has_value() )
            {
                impl->shutdown();
                return std::unexpected( std::move( source_result.error() ) );
            }

            return Daemon{ std::move( impl ) };
        }
        catch( const std::exception& exception )
        {
            return exception_failure( "daemon start", exception );
        }
        catch( ... )
        {
            return unknown_exception_failure( "daemon start" );
        }
    }

    void
    Daemon::shutdown() noexcept
    {
        if( impl_ != nullptr )
        {
            impl_->shutdown();
        }
    }

    grab::EventBus&
    Daemon::bus() noexcept
    {
        return impl_->bus();
    }

    const std::string&
    Daemon::endpoint() const noexcept
    {
        return impl_->endpoint();
    }

}    // namespace grab::service
