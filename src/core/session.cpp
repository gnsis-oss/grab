#include "core/reactor.hpp"
#include "grab/event_bus.hpp"
#include "grab/locator.hpp"
#include "grab/query.hpp"
#include "grab/result.hpp"
#include "grab/session.hpp"
#include "kernel/lifecycle/session_impl.hpp"
#include "kernel/lifecycle/startup_signal.hpp"

#include <atomic>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace grab
{
    namespace
    {

        constexpr std::string_view sessionClosedMessage = "session is closed";
        constexpr std::string_view threadStartStep      = "session reactor thread start";
        constexpr std::string_view reactorRunStep       = "session reactor run";

        [[nodiscard]]
        grab::Error
        internal_error( std::string_view step,
                        std::string_view message )
        {
            return grab::Error{
                .code       = grab::ErrorCode::InternalFault,
                .message    = std::string{ step } + ": " + std::string{ message },
                .capability = {},
                .target     = {},
                .attempts   = {},
            };
        }

        [[nodiscard]]
        grab::Error
        exception_error( std::string_view      step,
                         const std::exception& exception )
        {
            return internal_error( step, exception.what() );
        }

        [[nodiscard]]
        grab::Error
        unknown_exception_error( std::string_view step )
        {
            return internal_error( step, "unknown exception" );
        }

        [[nodiscard]]
        grab::Error
        session_closed_error()
        {
            return grab::Error{
                .code       = grab::ErrorCode::SessionClosed,
                .message    = std::string{ sessionClosedMessage },
                .capability = {},
                .target     = {},
                .attempts   = {},
            };
        }

    }    // namespace

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
            kernel::lifecycle::SessionCore*
            core() noexcept
            {
                return core_.get();
            }

        private:

            [[nodiscard]]
            grab::Result<void>
            run_reactor();

            void
                                                            join_thread() noexcept;

            SessionOptions                                  options_;
            grab::core::Reactor                             reactor_;
            std::thread                                     reactor_thread_;
            std::mutex                                      close_mutex_;
            std::unique_ptr<kernel::lifecycle::SessionCore> core_;
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
            return std::unexpected( exception_error( threadStartStep, exception ) );
        }
        catch( ... )
        {
            close();
            return std::unexpected( unknown_exception_error( threadStartStep ) );
        }

        auto start_result = ready.get();
        if( !start_result.has_value() )
        {
            auto error = std::move( start_result.error() );
            close();
            return std::unexpected( std::move( error ) );
        }

        // Compose the live stack when a display is available; without one the
        // session stays reactor-only (composition diagnostics land with the
        // AT-SPI attach task).
        if( auto core = kernel::lifecycle::SessionCore::open( options_ ) )
        {
            core_ = std::move( *core );
        }

        open_.store( true, std::memory_order_release );
        return {};
    }

    void
    Session::Impl::close() noexcept
    {
        const std::scoped_lock lock( close_mutex_ );
        if( open_.exchange( false, std::memory_order_acq_rel ) )
        {
            reactor_.stop();
        }
        join_thread();
        core_.reset();
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
            return std::unexpected( session_closed_error() );
        }

        reactor_.post( std::move( fn ) );
        return {};
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
            return std::unexpected( exception_error( reactorRunStep, exception ) );
        }
        catch( ... )
        {
            return std::unexpected( unknown_exception_error( reactorRunStep ) );
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
        return session;
    }

    Session::Session( SessionOptions options ) :
        impl_( std::make_unique<Impl>( std::move( options ) ) )
    {
    }

    Session::~Session()
    {
        close();
    }

    void
    Session::close() noexcept
    {
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

    grab::Result<Subscription>
    Session::watch( SubscriptionScope scope,
                    QueueOptions      options )
    {
        return kernel::lifecycle::watch_verb( impl_->core(),
                                              std::move( scope ),
                                              options );
    }

}    // namespace grab
