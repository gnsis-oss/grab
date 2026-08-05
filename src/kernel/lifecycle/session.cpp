#include "grab/capture.hpp"
#include "grab/context.hpp"
#include "grab/interaction.hpp"
#include "grab/locator.hpp"
#include "grab/overlay.hpp"
#include "grab/overlay_edit.hpp"
#include "grab/query.hpp"
#include "grab/result.hpp"
#include "grab/session.hpp"
#include "grab/space.hpp"
#include "grab/trace.hpp"
#include "grab/watch.hpp"
#include "kernel/events/event_bus.hpp"
#include "kernel/lifecycle/session_errors.hpp"
#include "kernel/lifecycle/session_impl.hpp"
#include "kernel/lifecycle/startup_signal.hpp"
#include "kernel/presentation/cursor_feedback.hpp"
#include "kernel/presentation/overlay_edit_session.hpp"
#include "kernel/presentation/overlay_service.hpp"
#include "kernel/scheduling/reactor.hpp"
#include "spi/runtime.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace grab
{

    namespace
    {

        constexpr std::string_view overlayDispatchStep = "overlay reactor dispatch";
        constexpr std::string_view cursorFeedbackStartupStep = "start cursor feedback";
        constexpr std::string_view cursorFeedbackTimerStep =
            "schedule cursor feedback timer";
        constexpr auto overlayClosePollInterval = std::chrono::milliseconds{ 16 };

        void
        stop_cursor_feedback_noexcept(
            const std::shared_ptr<kernel::presentation::CursorFeedbackObserver>& observer
        ) noexcept
        {
            if( observer == nullptr )
            {
                return;
            }
            try
            {
                [[maybe_unused]]
                auto stopped = observer->stop();
            }
            catch( ... )
            {
                return;
            }
        }

    }    // namespace

    namespace
    {

        // Pending off-thread invocations, cancellable at detach: if the reactor
        // exits before running a posted completion, detach() settles every
        // outstanding promise with SessionClosed instead of leaving callers
        // blocked forever. Each operation settles exactly once (CAS-guarded).
        class OverlayPendingRegistry final
        {
            public:

                [[nodiscard]]
                std::uint64_t
                add( std::function<void()> cancel )
                {
                    const std::scoped_lock lock{ mutex_ };
                    const auto             ticket = next_ticket_++;
                    pending_.emplace_back( ticket, std::move( cancel ) );
                    return ticket;
                }

                void
                remove( std::uint64_t ticket )
                {
                    const std::scoped_lock lock{ mutex_ };
                    std::erase_if( pending_,
                                   [ticket]( const auto& entry )
                                   {
                                       return entry.first == ticket;
                                   } );
                }

                void
                cancel_all()
                {
                    std::vector<std::pair<std::uint64_t, std::function<void()>>> drained;
                    {
                        const std::scoped_lock lock{ mutex_ };
                        drained.swap( pending_ );
                    }
                    for( auto& [ticket, cancel] : drained )
                    {
                        cancel();
                    }
                }

            private:

                std::mutex    mutex_;
                std::uint64_t next_ticket_{};
                std::vector<std::pair<std::uint64_t, std::function<void()>>> pending_;
        };

        // Shared between the facade and every posted execution: detach takes
        // the writer side to wait out in-flight executions, then clears the
        // alive flag so late-running posts settle SessionClosed instead of
        // touching a dying SessionCore.
        struct OverlayExecutionGate final
        {
                std::shared_mutex mutex;
                std::atomic_bool  alive{ true };
        };

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

                auto       completion = std::make_shared<std::promise<Result<T>>>();
                auto       completed  = completion->get_future();
                auto       settled    = std::make_shared<std::atomic_bool>( false );
                auto       registry   = registry_;
                const auto ticket     = registry->add(
                    [completion, settled]
                    {
                        bool expected = false;
                        if( settled->compare_exchange_strong( expected, true ) )
                        {
                            completion->set_value( std::unexpected(
                                kernel::lifecycle::session_closed_error()
                            ) );
                        }
                    }
                );
                auto gate = execution_gate_;
                reactor->post(
                    [completion,
                     settled,
                     registry,
                     gate,
                     ticket,
                     core,
                     operation = std::forward<Operation>( operation )]() mutable
                    {
                        // The shared gate keeps detach() from tearing the
                        // core down while this execution is in flight; the
                        // alive flag rejects executions that lost the race.
                        const std::shared_lock execution{ gate->mutex };
                        bool                   expected = false;
                        if( settled->compare_exchange_strong( expected, true ) )
                        {
                            if( gate->alive.load( std::memory_order_acquire ) )
                            {
                                completion->set_value(
                                    invoke_safely<T>( core, std::move( operation ) )
                                );
                            }
                            else
                            {
                                completion->set_value( std::unexpected(
                                    kernel::lifecycle::session_closed_error()
                                ) );
                            }
                        }
                        registry->remove( ticket );
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

            std::mutex                              mutex_;
            grab::core::Reactor*                    reactor_{};
            std::thread::id                         reactor_thread_;
            kernel::lifecycle::SessionCore*         core_{};
            std::shared_ptr<OverlayPendingRegistry> registry_ =
                std::make_shared<OverlayPendingRegistry>();
            std::shared_ptr<OverlayExecutionGate> execution_gate_ =
                std::make_shared<OverlayExecutionGate>();
    };

    class EditSession::Impl
    {
        public:

            Impl( std::shared_ptr<Overlay::Impl>                            dispatcher,
                  std::shared_ptr<kernel::presentation::OverlayEditSession> session ) :
                dispatcher_{ std::move( dispatcher ) },
                session_{ std::move( session ) }
            {
            }

            ~Impl() noexcept
            {
                if( dispatcher_ == nullptr || session_ == nullptr )
                {
                    return;
                }
                try
                {
                    [[maybe_unused]]
                    auto stopped = dispatcher_->invoke<void>(
                        [session =
                             session_]( kernel::presentation::OverlayService& service )
                        {
                            return service.stop_edit( session );
                        }
                    );
                }
                catch( ... )
                {
                    return;
                }
            }

            Impl( const Impl& ) = delete;
            Impl&
            operator=( const Impl& ) = delete;
            Impl( Impl&& )           = delete;
            Impl&
                                           operator=( Impl&& ) = delete;

            std::shared_ptr<Overlay::Impl> dispatcher_;
            std::shared_ptr<kernel::presentation::OverlayEditSession> session_;
    };

    Overlay::Overlay() :
        impl_( std::make_shared<Impl>() )
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

    Result<std::vector<overlay::ShapeId>>
    Overlay::add_many( std::span<overlay::Shape> shapes )
    {
        // ONE round trip for the whole batch — the point of the call.
        auto owned_shapes = std::vector<overlay::Shape>( shapes.begin(), shapes.end() );
        return impl_->invoke<std::vector<overlay::ShapeId>>(
            [shapes = std::move( owned_shapes )](
                kernel::presentation::OverlayService& service
            ) mutable
            {
                return service.add_many( shapes );
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

    EditSession::EditSession( std::unique_ptr<Impl> impl ) noexcept :
        impl_{ std::move( impl ) }
    {
    }

    EditSession::~EditSession()                        = default;

    EditSession::EditSession( EditSession&& ) noexcept = default;

    EditSession&
    EditSession::operator=( EditSession&& ) noexcept = default;

    Result<void>
    EditSession::status() const
    {
        if( impl_ == nullptr || impl_->session_ == nullptr )
        {
            return {};
        }
        return impl_->session_->status();
    }

    Result<EditSession>
    EditSession::create( Overlay&                          overlay,
                         std::span<const overlay::ShapeId> editable,
                         EditCallbacks                     callbacks )
    {
        auto owned_editable =
            std::vector<overlay::ShapeId>{ editable.begin(), editable.end() };
        auto dispatcher = overlay.impl_;
        auto started =
            dispatcher
                ->invoke<std::shared_ptr<kernel::presentation::OverlayEditSession>>(
                    [editable_ids = std::move( owned_editable ),
                     callbacks    = std::move( callbacks )](
                        kernel::presentation::OverlayService& service
                    ) mutable
                    {
                        return service.start_edit( editable_ids,
                                                   std::move( callbacks ) );
                    }
                );
        if( !started.has_value() )
        {
            return std::unexpected( std::move( started.error() ) );
        }

        try
        {
            return EditSession{ std::make_unique<Impl>( dispatcher, *started ) };
        }
        catch( const std::exception& exception )
        {
            [[maybe_unused]]
            auto stopped = dispatcher->invoke<void>(
                [session = *started]( kernel::presentation::OverlayService& service )
                {
                    return service.stop_edit( session );
                }
            );
            return std::unexpected(
                kernel::lifecycle::exception_error( overlayDispatchStep, exception )
            );
        }
        catch( ... )
        {
            [[maybe_unused]]
            auto stopped = dispatcher->invoke<void>(
                [session = *started]( kernel::presentation::OverlayService& service )
                {
                    return service.stop_edit( session );
                }
            );
            return std::unexpected(
                kernel::lifecycle::unknown_exception_error( overlayDispatchStep )
            );
        }
    }

    Result<EditSession>
    overlay_edit( Overlay&                          overlay,
                  std::span<const overlay::ShapeId> editable,
                  EditCallbacks                     callbacks )
    {
        return EditSession::create( overlay, editable, std::move( callbacks ) );
    }

    void
    Overlay::detach() noexcept
    {
        bool called_on_reactor{};
        {
            const std::scoped_lock lock{ impl_->mutex_ };
            called_on_reactor = std::this_thread::get_id() == impl_->reactor_thread_;
            impl_->core_      = nullptr;
            impl_->reactor_   = nullptr;
            impl_->reactor_thread_ = {};
        }
        // Refuse new executions, then wait out any execution already running
        // on the reactor thread before the SessionCore may be torn down.
        impl_->execution_gate_->alive.store( false, std::memory_order_release );
        if( !called_on_reactor )
        {
            const std::unique_lock drain{ impl_->execution_gate_->mutex };
        }
        // Reactor-thread close queues SessionCore teardown at the reactor tail.
        // It must not try to upgrade the shared gate held by the callback's
        // enclosing facade invocation; that invocation releases it on return.
        // Settle every invocation still waiting on a reactor that will never
        // run it; each settles exactly once.
        impl_->registry_->cancel_all();
    }

    class CursorFeedback::Impl
    {
        public:

            explicit Impl(
                std::shared_ptr<kernel::presentation::CursorFeedbackObserver> observer
            ) :
                observer_{ std::move( observer ) }
            {
            }

            ~Impl() noexcept
            {
                stop_cursor_feedback_noexcept( observer_ );
            }

            Impl( const Impl& ) = delete;
            Impl&
            operator=( const Impl& ) = delete;
            Impl( Impl&& )           = delete;
            Impl&
            operator=( Impl&& ) = delete;

            std::shared_ptr<kernel::presentation::CursorFeedbackObserver> observer_;
    };

    CursorFeedback::CursorFeedback( std::unique_ptr<Impl> impl ) noexcept :
        impl_{ std::move( impl ) }
    {
    }

    CursorFeedback::~CursorFeedback()                           = default;

    CursorFeedback::CursorFeedback( CursorFeedback&& ) noexcept = default;

    CursorFeedback&
    CursorFeedback::operator=( CursorFeedback&& ) noexcept = default;

    Result<void>
    CursorFeedback::status() const
    {
        if( impl_ == nullptr || impl_->observer_ == nullptr )
        {
            return {};
        }
        return impl_->observer_->status();
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

            [[nodiscard]]
            Result<void>
            start_manual_observation();

            void
            stop_manual_observation() noexcept;

            [[nodiscard]]
            Result<void>
            acquire_feedback_observation();

            [[nodiscard]]
            Result<void>
            release_feedback_observation();

            [[nodiscard]]
            Result<std::shared_ptr<kernel::presentation::CursorFeedbackObserver>>
            start_feedback( CursorFeedbackConfig config,
                            Overlay&             overlay );

            [[nodiscard]]
            kernel::presentation::CursorFeedbackObserverHooks
            feedback_hooks( Overlay& overlay );

            void
            stop_feedback() noexcept;

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
            stop_feedback_observation_if_unused() noexcept;

            void
            reset_core() noexcept;

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
            std::atomic_bool                                reactor_running_{ false };
            std::mutex                                      observation_mutex_;
            bool                                            manual_observation_{};
            std::size_t feedback_observation_count_{};
            std::mutex  feedback_mutex_;
            bool        feedback_closing_{};
            std::vector<std::weak_ptr<kernel::presentation::CursorFeedbackObserver>>
                feedback_;
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
                    reactor_running_.store( true, std::memory_order_release );
                    auto result = run_reactor();
                    reactor_running_.store( false, std::memory_order_release );
                    startup->report( std::move( result ) );
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
            injected_runtime_->bind_reactor( &reactor_ );
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
        reset_core();
    }

    void
    Session::Impl::finish_close_on_reactor() noexcept
    {
        close_overlay_on_reactor();
        reset_core();
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
        if( !is_open() || !reactor_running_.load( std::memory_order_acquire ) )
        {
            return std::unexpected( kernel::lifecycle::session_closed_error() );
        }

        reactor_.post( std::move( fn ) );
        return {};
    }

    Result<void>
    Session::Impl::start_manual_observation()
    {
        const std::scoped_lock observation_lock{ observation_mutex_ };
        if( !is_open() )
        {
            return std::unexpected( kernel::lifecycle::session_closed_error() );
        }
        if( core_ == nullptr )
        {
            return grab::fail( grab::ErrorCode::CapabilityUnavailable,
                               "session has no composed runtime for observation" );
        }
        if( manual_observation_ )
        {
            return {};
        }
        if( feedback_observation_count_ == 0U )
        {
            auto started = core_->start_observation( grab::OperationContext{} );
            if( !started.has_value() )
            {
                return started;
            }
        }
        manual_observation_ = true;
        return {};
    }

    void
    Session::Impl::stop_manual_observation() noexcept
    {
        try
        {
            const std::scoped_lock observation_lock{ observation_mutex_ };
            if( !manual_observation_ )
            {
                return;
            }
            manual_observation_ = false;
            if( feedback_observation_count_ == 0U && core_ != nullptr )
            {
                core_->stop_observation();
            }
        }
        catch( ... )
        {
            return;
        }
    }

    Result<void>
    Session::Impl::acquire_feedback_observation()
    {
        try
        {
            const std::scoped_lock observation_lock{ observation_mutex_ };
            if( !is_open() )
            {
                return std::unexpected( kernel::lifecycle::session_closed_error() );
            }
            if( core_ == nullptr )
            {
                return grab::fail( grab::ErrorCode::CapabilityUnavailable,
                                   "session has no composed runtime for observation" );
            }
            if( feedback_observation_count_ ==
                std::numeric_limits<decltype( feedback_observation_count_ )>::max() )
            {
                return grab::fail( grab::ErrorCode::Overflowed,
                                   "cursor feedback observation count is exhausted" );
            }
            if( !manual_observation_ && feedback_observation_count_ == 0U )
            {
                try
                {
                    auto started = core_->start_observation( grab::OperationContext{} );
                    if( !started.has_value() )
                    {
                        return started;
                    }
                }
                catch( const std::exception& exception )
                {
                    core_->stop_observation();
                    return std::unexpected(
                        kernel::lifecycle::exception_error( cursorFeedbackStartupStep,
                                                            exception )
                    );
                }
                catch( ... )
                {
                    core_->stop_observation();
                    return std::unexpected( kernel::lifecycle::unknown_exception_error(
                        cursorFeedbackStartupStep
                    ) );
                }
            }
            ++feedback_observation_count_;
            return {};
        }
        catch( const std::exception& exception )
        {
            return std::unexpected(
                kernel::lifecycle::exception_error( cursorFeedbackStartupStep,
                                                    exception )
            );
        }
        catch( ... )
        {
            return std::unexpected(
                kernel::lifecycle::unknown_exception_error( cursorFeedbackStartupStep )
            );
        }
    }

    Result<void>
    Session::Impl::release_feedback_observation()
    {
        try
        {
            {
                const std::scoped_lock observation_lock{ observation_mutex_ };
                if( feedback_observation_count_ == 0U )
                {
                    return {};
                }
                --feedback_observation_count_;
                if( feedback_observation_count_ !=
                    0U ||
                    manual_observation_ ||
                    core_ == nullptr )
                {
                    return {};
                }
            }

            return post(
                [this]
                {
                    stop_feedback_observation_if_unused();
                }
            );
        }
        catch( const std::exception& exception )
        {
            return std::unexpected(
                kernel::lifecycle::exception_error( cursorFeedbackStartupStep,
                                                    exception )
            );
        }
        catch( ... )
        {
            return std::unexpected(
                kernel::lifecycle::unknown_exception_error( cursorFeedbackStartupStep )
            );
        }
    }

    void
    Session::Impl::stop_feedback_observation_if_unused() noexcept
    {
        try
        {
            const std::scoped_lock observation_lock{ observation_mutex_ };
            if( feedback_observation_count_ ==
                0U &&
                !manual_observation_ &&
                core_ != nullptr )
            {
                core_->stop_observation();
            }
        }
        catch( ... )
        {
            return;
        }
    }

    kernel::presentation::CursorFeedbackObserverHooks
    Session::Impl::feedback_hooks( Overlay& overlay_facade )
    {
        return kernel::presentation::CursorFeedbackObserverHooks{
            .bus = &bus(),
            .post =
                [this]( std::function<void()> fn )
            {
                return post( std::move( fn ) );
            },
            .schedule = [this]( std::chrono::nanoseconds delay,
                                std::function<void()>    callback ) -> Result<void>
            {
                if( !reactor_running_.load( std::memory_order_acquire ) )
                {
                    return std::unexpected( kernel::lifecycle::session_closed_error() );
                }
                try
                {
                    static_cast<void>( reactor().add_timer( delay,
                                                            std::move( callback ) ) );
                    return {};
                }
                catch( const std::exception& exception )
                {
                    return std::unexpected(
                        kernel::lifecycle::exception_error( cursorFeedbackTimerStep,
                                                            exception )
                    );
                }
                catch( ... )
                {
                    return std::unexpected( kernel::lifecycle::unknown_exception_error(
                        cursorFeedbackTimerStep
                    ) );
                }
            },
            .clock =
                []
            {
                return std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                );
            },
            .add_shape =
                [&overlay_facade]( overlay::Shape shape )
            {
                return overlay_facade.add( std::move( shape ) );
            },
            .remove_shape =
                [&overlay_facade]( overlay::ShapeId id )
            {
                return overlay_facade.remove( id );
            },
            .on_reactor_thread =
                [this]
            {
                return std::this_thread::get_id() == reactor_thread_id();
            },
            .reactor_alive =
                [this]
            {
                return reactor_running_.load( std::memory_order_acquire );
            },
            .release_observation = [this]() -> Result<void>
            {
                return release_feedback_observation();
            },
        };
    }

    Result<std::shared_ptr<kernel::presentation::CursorFeedbackObserver>>
    Session::Impl::start_feedback( CursorFeedbackConfig config,
                                   Overlay&             overlay_facade )
    {
        const std::scoped_lock feedback_lock{ feedback_mutex_ };
        if( feedback_closing_ || !is_open() )
        {
            return std::unexpected( kernel::lifecycle::session_closed_error() );
        }

        bool observation_acquired = false;
        std::shared_ptr<kernel::presentation::CursorFeedbackObserver> observer;
        try
        {
            auto observation = acquire_feedback_observation();
            if( !observation.has_value() )
            {
                return std::unexpected( std::move( observation.error() ) );
            }
            observation_acquired = true;

            auto started         = kernel::presentation::CursorFeedbackObserver::start(
                config,
                feedback_hooks( overlay_facade )
            );
            if( !started.has_value() )
            {
                [[maybe_unused]]
                auto released        = release_feedback_observation();
                observation_acquired = false;
                return std::unexpected( std::move( started.error() ) );
            }

            observer             = std::move( *started );
            observation_acquired = false;
            std::erase_if( feedback_,
                           []( const auto& candidate )
                           {
                               return candidate.expired();
                           } );
            feedback_.push_back( observer );
            return observer;
        }
        catch( const std::exception& exception )
        {
            if( observer != nullptr )
            {
                stop_cursor_feedback_noexcept( observer );
            }
            else if( observation_acquired )
            {
                [[maybe_unused]]
                auto released = release_feedback_observation();
            }
            return std::unexpected(
                kernel::lifecycle::exception_error( cursorFeedbackStartupStep,
                                                    exception )
            );
        }
        catch( ... )
        {
            if( observer != nullptr )
            {
                stop_cursor_feedback_noexcept( observer );
            }
            else if( observation_acquired )
            {
                [[maybe_unused]]
                auto released = release_feedback_observation();
            }
            return std::unexpected(
                kernel::lifecycle::unknown_exception_error( cursorFeedbackStartupStep )
            );
        }
    }

    void
    Session::Impl::stop_feedback() noexcept
    {
        std::vector<std::weak_ptr<kernel::presentation::CursorFeedbackObserver>>
            observers;
        try
        {
            {
                const std::scoped_lock feedback_lock{ feedback_mutex_ };
                feedback_closing_ = true;
                observers.swap( feedback_ );
            }
            for( const auto& candidate : observers )
            {
                if( auto observer = candidate.lock() )
                {
                    stop_cursor_feedback_noexcept( observer );
                }
            }
        }
        catch( ... )
        {
            return;
        }
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
        if( !reactor_running_.load( std::memory_order_acquire ) )
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
            while( completed.wait_for( overlayClosePollInterval ) !=
                   std::future_status::ready )
            {
                if( !reactor_running_.load( std::memory_order_acquire ) )
                {
                    core_->close_overlay();
                    return;
                }
            }
        }
        catch( ... )
        {
            // Session teardown must still release the lease if dispatch setup
            // itself cannot allocate. Runtime shutdown remains best-effort.
            core_->close_overlay();
        }
    }

    void
    Session::Impl::reset_core() noexcept
    {
        std::unique_ptr<kernel::lifecycle::SessionCore> core;
        try
        {
            {
                const std::scoped_lock observation_lock{ observation_mutex_ };
                manual_observation_         = false;
                feedback_observation_count_ = 0U;
                core                        = std::move( core_ );
            }
            core.reset();
        }
        catch( ... )
        {
            return;
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
        impl_->stop_feedback();
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

    grab::Result<std::vector<Match>>
    Session::resolve_all( const Locator& locator )
    {
        return kernel::lifecycle::resolve_all_verb( impl_->core(), locator );
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

    Result<CursorFeedback>
    Session::cursor_feedback( CursorFeedbackConfig config )
    {
        auto valid = kernel::presentation::validate_cursor_feedback_config( config );
        if( !valid.has_value() )
        {
            return std::unexpected( std::move( valid.error() ) );
        }
        auto overlay_result = overlay();
        if( !overlay_result.has_value() )
        {
            return std::unexpected( std::move( overlay_result.error() ) );
        }

        auto observer = impl_->start_feedback( config, **overlay_result );
        if( !observer.has_value() )
        {
            return std::unexpected( std::move( observer.error() ) );
        }

        try
        {
            return CursorFeedback{
                std::make_unique<CursorFeedback::Impl>( std::move( *observer ) )
            };
        }
        catch( const std::exception& exception )
        {
            stop_cursor_feedback_noexcept( *observer );
            return std::unexpected(
                kernel::lifecycle::exception_error( cursorFeedbackStartupStep,
                                                    exception )
            );
        }
        catch( ... )
        {
            stop_cursor_feedback_noexcept( *observer );
            return std::unexpected(
                kernel::lifecycle::unknown_exception_error( cursorFeedbackStartupStep )
            );
        }
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
        return impl_->start_manual_observation();
    }

    void
    Session::stop_observation() noexcept
    {
        impl_->stop_manual_observation();
    }

    grab::Result<void>
    Session::resync()
    {
        auto* const core = impl_->core();
        if( core == nullptr )
        {
            return grab::fail( grab::ErrorCode::CapabilityUnavailable,
                               "session has no composed runtime to resync" );
        }
        return core->resync( grab::OperationContext{} );
    }

}    // namespace grab
