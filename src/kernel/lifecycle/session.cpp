#include "grab/capture.hpp"
#include "grab/context.hpp"
#include "grab/interaction.hpp"
#include "grab/locator.hpp"
#include "grab/overlay.hpp"
#include "grab/query.hpp"
#include "grab/result.hpp"
#include "grab/session.hpp"
#include "grab/trace.hpp"
#include "grab/watch.hpp"
#include "kernel/events/event_bus.hpp"
#include "kernel/lifecycle/session_errors.hpp"
#include "kernel/lifecycle/session_impl.hpp"
#include "kernel/lifecycle/startup_signal.hpp"
#include "kernel/presentation/overlay_service.hpp"
#include "kernel/scheduling/reactor.hpp"
#include "spi/runtime.hpp"

#include <atomic>
#include <exception>
#include <expected>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>

namespace grab
{

    namespace
    {

        constexpr std::string_view overlayDispatchStep = "overlay reactor dispatch";

    }    // namespace

    class Overlay::Impl
    {
        public:

            template<typename T,
                     typename Operation>
            [[nodiscard]]
            Result<T>
            invoke( Operation&& operation )
            {
                std::unique_lock lock{ mutex_ };
                if( reactor_ == nullptr )
                {
                    return std::unexpected( kernel::lifecycle::session_closed_error() );
                }
                auto* const core    = core_;
                auto* const reactor = reactor_;
                if( std::this_thread::get_id() == reactor_thread_ )
                {
                    lock.unlock();
                    return invoke_safely<T>( core,
                                             std::forward<Operation>( operation ) );
                }

                auto completion = std::make_shared<std::promise<Result<T>>>();
                auto completed  = completion->get_future();
                reactor->post(
                    [completion,
                     core,
                     operation = std::forward<Operation>( operation )]() mutable
                    {
                        completion->set_value(
                            invoke_safely<T>( core, std::move( operation ) )
                        );
                    }
                );
                lock.unlock();
                return completed.get();
            }

        private:

            friend class Overlay;
            friend class Session;

            template<typename T,
                     typename Operation>
            [[nodiscard]]
            static Result<T>
            invoke_safely( kernel::lifecycle::SessionCore* core,
                           Operation&&                     operation )
            {
                try
                {
                    auto service = kernel::lifecycle::overlay_verb( core );
                    if( !service.has_value() )
                    {
                        return std::unexpected( std::move( service.error() ) );
                    }
                    return std::forward<Operation>( operation )( **service );
                }
                catch( const std::exception& exception )
                {
                    return std::unexpected(
                        kernel::lifecycle::exception_error( overlayDispatchStep,
                                                            exception )
                    );
                }
                catch( ... )
                {
                    return std::unexpected(
                        kernel::lifecycle::unknown_exception_error( overlayDispatchStep )
                    );
                }
            }

            std::mutex                      mutex_;
            grab::core::Reactor*            reactor_{};
            std::thread::id                 reactor_thread_;
            kernel::lifecycle::SessionCore* core_{};
    };

    Overlay::Overlay() :
        impl_( std::make_unique<Impl>() )
    {
    }

    Overlay::~Overlay() = default;

    Result<overlay::ShapeId>
    Overlay::add( overlay::Shape shape )
    {
        return impl_->invoke<overlay::ShapeId>(
            [shape = std::move( shape )](
                kernel::presentation::OverlayService& service
            ) mutable
            {
                return service.add( std::move( shape ) );
            }
        );
    }

    Result<void>
    Overlay::update( overlay::ShapeId id,
                     overlay::Shape   shape )
    {
        return impl_->invoke<void>(
            [id, shape = std::move( shape )](
                kernel::presentation::OverlayService& service
            ) mutable
            {
                return service.update( id, std::move( shape ) );
            }
        );
    }

    Result<void>
    Overlay::remove( overlay::ShapeId id )
    {
        return impl_->invoke<void>(
            [id]( kernel::presentation::OverlayService& service )
            {
                return service.remove( id );
            }
        );
    }

    void
    Overlay::clear()
    {
        auto result = impl_->invoke<void>(
            []( kernel::presentation::OverlayService& service ) -> Result<void>
            {
                service.clear();
                return {};
            }
        );
        static_cast<void>( result );
    }

    Result<void>
    Overlay::flush()
    {
        return impl_->invoke<void>(
            []( kernel::presentation::OverlayService& service )
            {
                return service.flush();
            }
        );
    }

    Result<CoordinateSpaceId>
    Overlay::space()
    {
        return impl_->invoke<CoordinateSpaceId>(
            []( kernel::presentation::OverlayService& service )
            {
                return Result<CoordinateSpaceId>{ service.delegate_space() };
            }
        );
    }

    void
    Overlay::detach() noexcept
    {
        const std::scoped_lock lock{ impl_->mutex_ };
        impl_->core_           = nullptr;
        impl_->reactor_        = nullptr;
        impl_->reactor_thread_ = {};
    }

    class Session::Impl
    {
        public:

            explicit Impl( SessionOptions options );
            ~Impl()             = default;

            Impl( const Impl& ) = delete;
            Impl&
            operator=( const Impl& ) = delete;
            Impl( Impl&& )           = delete;
            Impl&
            operator=( Impl&& ) = delete;

            [[nodiscard]]
            grab::Result<void>
            start();

            void
            close() noexcept;

            [[nodiscard]]
            bool
            is_open() const noexcept;

            [[nodiscard]]
            grab::core::Reactor&
            reactor() noexcept;

            [[nodiscard]]
            grab::Result<void>
            post( std::function<void()> fn );

            [[nodiscard]]
            std::thread::id
            reactor_thread_id() const noexcept
            {
                return reactor_thread_.get_id();
            }

            [[nodiscard]]
            kernel::lifecycle::SessionCore*
            core() noexcept
            {
                return core_.get();
            }

            [[nodiscard]]
            grab::EventBus&
            bus() noexcept
            {
                return core_ != nullptr ? core_->bus() : fallback_bus_;
            }

            void
            set_injected_runtime( std::unique_ptr<spi::Runtime> runtime )
            {
                injected_runtime_ = std::move( runtime );
            }

            [[nodiscard]]
            Overlay*
            overlay_facade() noexcept
            {
                return overlay_.get();
            }

            void
            set_overlay_facade( std::unique_ptr<Overlay> overlay )
            {
                overlay_ = std::move( overlay );
            }

        private:

            [[nodiscard]]
            grab::Result<void>
            run_reactor();

            void
            close_overlay_on_reactor() noexcept;

            void
            close_locked() noexcept;

            void
            finish_close_on_reactor() noexcept;

            void
                                                            join_thread() noexcept;

            SessionOptions                                  options_;
            grab::core::Reactor                             reactor_;
            std::thread                                     reactor_thread_;
            std::mutex                                      close_mutex_;
            grab::EventBus                                  fallback_bus_;
            std::unique_ptr<spi::Runtime>                   injected_runtime_;
            std::unique_ptr<kernel::lifecycle::SessionCore> core_;
            std::unique_ptr<Overlay>                        overlay_;
            std::atomic_bool                                open_{ false };
    };

    Session::Impl::Impl( SessionOptions options ) :
        options_( std::move( options ) )
    {
    }

    grab::Result<void>
    Session::Impl::start()
    {
        const auto startup = std::make_shared<kernel::lifecycle::StartupSignal>();
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
            close();
            return std::unexpected(
                kernel::lifecycle::exception_error( kernel::lifecycle::threadStartStep,
                                                    exception )
            );
        }
        catch( ... )
        {
            close();
            return std::unexpected( kernel::lifecycle::unknown_exception_error(
                kernel::lifecycle::threadStartStep
            ) );
        }

        auto start_result = ready.get();
        if( !start_result.has_value() )
        {
            auto error = std::move( start_result.error() );
            close();
            return std::unexpected( std::move( error ) );
        }

        // Compose the live stack. An injected runtime (observation/daemon seam)
        // takes precedence; otherwise compose the display stack when available.
        // Without either, the session stays reactor-only.
        if( injected_runtime_ != nullptr )
        {
            if( auto core = kernel::lifecycle::SessionCore::open_owning(
                    std::move( injected_runtime_ ),
                    grab::OperationContext{}
                ) )
            {
                core_ = std::move( *core );
            }
        }
        else if( auto core =
                     kernel::lifecycle::SessionCore::open( options_, &reactor_ ) )
        {
            core_ = std::move( *core );
        }

        open_.store( true, std::memory_order_release );
        return {};
    }

    void
    Session::Impl::close() noexcept
    {
        if( std::this_thread::get_id() == reactor_thread_.get_id() )
        {
            std::unique_lock lock{ close_mutex_, std::try_to_lock };
            if( !lock.owns_lock() )
            {
                return;
            }
            close_locked();
            return;
        }

        const std::scoped_lock lock{ close_mutex_ };
        close_locked();
    }

    void
    Session::Impl::close_locked() noexcept
    {
        if( std::this_thread::get_id() == reactor_thread_.get_id() )
        {
            if( !open_.exchange( false, std::memory_order_acq_rel ) )
            {
                return;
            }
            try
            {
                // A facade call may already be queued behind the callback that
                // requested close. Put teardown at the queue tail so that call
                // can complete instead of leaving its caller blocked forever.
                reactor_.post(
                    [this]
                    {
                        finish_close_on_reactor();
                    }
                );
            }
            catch( ... )
            {
                finish_close_on_reactor();
            }
            return;
        }

        if( open_.exchange( false, std::memory_order_acq_rel ) )
        {
            close_overlay_on_reactor();
            reactor_.stop();
        }
        join_thread();
        core_.reset();
    }

    void
    Session::Impl::finish_close_on_reactor() noexcept
    {
        close_overlay_on_reactor();
        core_.reset();
        reactor_.stop();
    }

    bool
    Session::Impl::is_open() const noexcept
    {
        return open_.load( std::memory_order_acquire );
    }

    grab::core::Reactor&
    Session::Impl::reactor() noexcept
    {
        return reactor_;
    }

    grab::Result<void>
    Session::Impl::post( std::function<void()> fn )
    {
        if( !is_open() )
        {
            return std::unexpected( kernel::lifecycle::session_closed_error() );
        }

        reactor_.post( std::move( fn ) );
        return {};
    }

    void
    Session::Impl::close_overlay_on_reactor() noexcept
    {
        if( core_ == nullptr )
        {
            return;
        }
        if( std::this_thread::get_id() == reactor_thread_.get_id() )
        {
            core_->close_overlay();
            return;
        }

        try
        {
            auto        completion = std::make_shared<std::promise<void>>();
            auto        completed  = completion->get_future();
            auto* const core       = core_.get();
            reactor_.post(
                [completion, core]
                {
                    core->close_overlay();
                    completion->set_value();
                }
            );
            completed.wait();
        }
        catch( ... )
        {
            // Session teardown must still release the lease if dispatch setup
            // itself cannot allocate. Runtime shutdown remains best-effort.
            core_->close_overlay();
        }
    }

    grab::Result<void>
    Session::Impl::run_reactor()
    {
        try
        {
            return reactor_.run();
        }
        catch( const std::exception& exception )
        {
            return std::unexpected(
                kernel::lifecycle::exception_error( kernel::lifecycle::reactorRunStep,
                                                    exception )
            );
        }
        catch( ... )
        {
            return std::unexpected( kernel::lifecycle::unknown_exception_error(
                kernel::lifecycle::reactorRunStep
            ) );
        }
    }

    void
    Session::Impl::join_thread() noexcept
    {
        if( !reactor_thread_.joinable() )
        {
            return;
        }
        if( reactor_thread_.get_id() == std::this_thread::get_id() )
        {
            return;
        }
        reactor_thread_.join();
    }

    grab::Result<std::unique_ptr<Session>>
    Session::open( SessionOptions options )
    {
        auto session = std::unique_ptr<Session>( new Session( std::move( options ) ) );
        if( auto result = session->impl_->start(); !result.has_value() )
        {
            return std::unexpected( std::move( result.error() ) );
        }
        session->bind_overlay_facade();
        return session;
    }

    Session::Session( SessionOptions options ) :
        impl_( std::make_unique<Impl>( std::move( options ) ) )
    {
        impl_->set_overlay_facade( std::unique_ptr<Overlay>( new Overlay() ) );
    }

    void
    Session::bind_overlay_facade() noexcept
    {
        auto* const            facade = impl_->overlay_facade();
        const std::scoped_lock lock{ facade->impl_->mutex_ };
        facade->impl_->core_           = impl_->core();
        facade->impl_->reactor_        = &impl_->reactor();
        facade->impl_->reactor_thread_ = impl_->reactor_thread_id();
    }

    Session::~Session()
    {
        close();
    }

    void
    Session::close() noexcept
    {
        if( auto* const overlay = impl_->overlay_facade() )
        {
            overlay->detach();
        }
        impl_->close();
    }

    bool
    Session::is_open() const noexcept
    {
        return impl_->is_open();
    }

    grab::core::Reactor&
    Session::reactor() noexcept
    {
        return impl_->reactor();
    }

    grab::Result<void>
    Session::post( std::function<void()> fn )
    {
        return impl_->post( std::move( fn ) );
    }

    grab::Result<Match>
    Session::resolve( const Locator& locator,
                      Cardinality    cardinality )
    {
        return kernel::lifecycle::resolve_verb( impl_->core(), locator, cardinality );
    }

    grab::Result<NodeInfo>
    Session::describe( const Match& match )
    {
        return kernel::lifecycle::describe_verb( impl_->core(), match );
    }

    grab::Result<Subscription>
    Session::watch( SubscriptionScope scope,
                    QueueOptions      options )
    {
        return kernel::lifecycle::watch_verb( impl_->core(),
                                              std::move( scope ),
                                              options );
    }

    grab::Result<Receipt>
    Session::perform( const Action&        action,
                      const ActionOptions& options )
    {
        return kernel::lifecycle::perform_verb( impl_->core(), action, options );
    }

    grab::Result<Frame>
    Session::capture( const CaptureTarget& target,
                      CaptureOptions       options )
    {
        return kernel::lifecycle::capture_verb( impl_->core(), target, options );
    }

    Result<Overlay*>
    Session::overlay()
    {
        auto* const facade    = impl_->overlay_facade();
        auto        available = facade->impl_->invoke<void>(
            []( kernel::presentation::OverlayService& ) -> Result<void>
            {
                return {};
            }
        );
        if( !available.has_value() )
        {
            return std::unexpected( std::move( available.error() ) );
        }
        return facade;
    }

    grab::Result<std::unique_ptr<Session>>
    Session::open_owning_runtime( std::unique_ptr<grab::spi::Runtime> runtime )
    {
        auto session = std::unique_ptr<Session>( new Session( SessionOptions{} ) );
        session->impl_->set_injected_runtime( std::move( runtime ) );
        if( auto result = session->impl_->start(); !result.has_value() )
        {
            return std::unexpected( std::move( result.error() ) );
        }
        session->bind_overlay_facade();
        return session;
    }

    EventBus&
    Session::bus() noexcept
    {
        return impl_->bus();
    }

    grab::Result<void>
    Session::start_observation()
    {
        auto* const core = impl_->core();
        if( core == nullptr )
        {
            return grab::fail( grab::ErrorCode::CapabilityUnavailable,
                               "session has no composed runtime for observation" );
        }
        return core->start_observation( grab::OperationContext{} );
    }

    void
    Session::stop_observation() noexcept
    {
        if( auto* const core = impl_->core() )
        {
            core->stop_observation();
        }
    }

}    // namespace grab
