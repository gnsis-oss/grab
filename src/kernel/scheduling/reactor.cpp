#include "grab/result.hpp"
#include "kernel/scheduling/reactor.hpp"
#include "kernel/support/log.hpp"
#include "kernel/support/log_tags.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <exception>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace grab::core
{
    namespace
    {

        constexpr int           invalidFd         = -1;
        constexpr int           posixFailure      = -1;
        constexpr int           posixSuccess      = 0;
        constexpr int           infiniteWait      = -1;
        constexpr int           noWait            = 0;
        constexpr int           firstReadyIndex   = 0;
        constexpr std::uint32_t noFdEvents        = 0U;
        constexpr std::uint64_t wakeToken         = 0U;
        constexpr std::uint64_t firstToken        = 1U;
        constexpr std::uint64_t tokenStep         = 1U;
        constexpr std::size_t   maxReadyEvents    = 64U;
        constexpr eventfd_t     emptyEventfdValue = 0U;
        constexpr eventfd_t     wakeEventfdValue  = 1U;

        // Reserved epoll `data.u64` for the deadline timerfd. Client tokens
        // come from a monotonic counter starting at `firstToken`, so the top
        // of the range is unreachable and cannot collide with one.
        constexpr std::uint64_t timerToken = std::numeric_limits<std::uint64_t>::max();

        constexpr std::uint64_t noExpirations = 0U;
        constexpr ssize_t       noBytesRead   = 0;

        // `timerfd_settime` reads an all-zero `it_value` as *disarm*, so a
        // deadline that lands exactly on the monotonic epoch has to be nudged
        // to the smallest representable non-zero instant to still arm.
        constexpr long          leastArmedNanoseconds = 1;

        [[nodiscard]]
        constexpr int
        eventfd_flags() noexcept
        {
            return static_cast<int>( static_cast<unsigned int>( EFD_CLOEXEC ) |
                                     static_cast<unsigned int>( EFD_NONBLOCK ) );
        }

        [[nodiscard]]
        constexpr int
        timerfd_flags() noexcept
        {
            return static_cast<int>( static_cast<unsigned int>( TFD_CLOEXEC ) |
                                     static_cast<unsigned int>( TFD_NONBLOCK ) );
        }

        [[nodiscard]]
        grab::Error
        posix_error( std::string_view step,
                     int              error_number )
        {
            return grab::Error{
                .code = grab::ErrorCode::InternalFault,
                .message =
                    std::string{ step }
                    +
                    ": " +
                    std::error_code{ error_number, std::generic_category() }
                    .message(),
                .capability = {},
                .target     = {},
                .attempts   = {},
            };
        }

        [[nodiscard]]
        grab::Error
        message_error( std::string message )
        {
            return grab::Error{
                .code       = grab::ErrorCode::InternalFault,
                .message    = std::move( message ),
                .capability = {},
                .target     = {},
                .attempts   = {},
            };
        }

        [[nodiscard]]
        grab::Error
        callback_error( std::string_view      step,
                        const std::exception& exception )
        {
            return message_error( std::string{ step } + ": " + exception.what() );
        }

    }    // namespace

    class Reactor::Impl
    {
        public:

            Impl();
            ~Impl() noexcept;

            Impl( const Impl& ) = delete;
            Impl&
            operator=( const Impl& ) = delete;
            Impl( Impl&& )           = delete;
            Impl&
            operator=( Impl&& ) = delete;

            [[nodiscard]]
            grab::Result<void>
            run();

            void
            stop() noexcept;

            [[nodiscard]]
            std::uint64_t
            add_fd( int                                  fd,
                    std::uint32_t                        events,
                    std::function<void( std::uint32_t )> cb );

            void
            remove_fd( std::uint64_t token );

            [[nodiscard]]
            std::uint64_t
            add_timer( std::chrono::nanoseconds delay,
                       std::function<void()>    cb );

            void
            post( std::function<void()> fn );

        private:

            enum class PendingKind : std::uint8_t
            {
                AddFd,
                RemoveFd,
                AddTimer,
                Task,
            };

            struct PendingOp
            {
                    PendingKind                          kind   = PendingKind::Task;
                    std::uint64_t                        token  = wakeToken;
                    int                                  fd     = invalidFd;
                    std::uint32_t                        events = noFdEvents;
                    std::chrono::nanoseconds             delay{};
                    std::function<void( std::uint32_t )> fd_callback;
                    std::function<void()>                void_callback;
            };

            struct FdRegistration
            {
                    std::uint64_t                        token = wakeToken;
                    int                                  fd    = invalidFd;
                    std::function<void( std::uint32_t )> callback;
            };

            struct TimerRegistration
            {
                    using TimePoint = std::chrono::steady_clock::time_point;

                    std::uint64_t         token;
                    TimePoint             deadline;
                    std::function<void()> callback;
            };

            struct TimerLater
            {
                    [[nodiscard]]
                    bool
                    operator()( const TimerRegistration& lhs,
                                const TimerRegistration& rhs ) const noexcept
                    {
                        return lhs.deadline > rhs.deadline;
                    }
            };

            void
            enqueue( PendingOp op );

            void
            wake() const noexcept;

            void
            drain_wake_fd() const noexcept;

            [[nodiscard]]
            grab::Result<void>
            drain_pending_ops();

            [[nodiscard]]
            grab::Result<void>
            add_fd_on_reactor( PendingOp& op );

            [[nodiscard]]
            grab::Result<void>
            remove_fd_on_reactor( std::uint64_t token );

            void
            add_timer_on_reactor( PendingOp op );

            void
            dispatch_expired_timers();

            void
            dispatch_ready_event( const epoll_event& event );

            // Arms `timer_fd_` at the nearest deadline, or disarms it when no
            // timer is pending. False means the arm failed and the caller must
            // fall back to a finite epoll timeout.
            [[nodiscard]]
            bool
            arm_timer_fd() const;

            void
            drain_timer_fd() const noexcept;

            // What `epoll_wait` is given. `infiniteWait` once the timerfd
            // holds the deadline; the millisecond fallback when it does not.
            [[nodiscard]]
            int
            wait_timeout() const;

            // Millisecond fallback, used only when the timerfd is
            // unavailable. `ceil` to milliseconds is exactly the precision
            // loss the timerfd exists to avoid, so this is a degraded mode,
            // not the normal path.
            [[nodiscard]]
            int
            epoll_timeout() const;

            [[nodiscard]]
            bool
                                        consume_cancelled_token( std::uint64_t token );

            int                         epoll_fd_ = invalidFd;
            int                         wake_fd_  = invalidFd;
            int                         timer_fd_ = invalidFd;
            std::mutex                  mutex_;
            std::vector<PendingOp>      pending_ops_;
            std::vector<FdRegistration> fds_;
            std::vector<TimerRegistration>          timers_;
            std::vector<std::uint64_t>              cancelled_tokens_;
            std::atomic_bool                        stop_requested_{ false };
            std::atomic<std::uint64_t>              next_token_{ firstToken };
            std::optional<grab::Error>              startup_error_;
            std::array<epoll_event, maxReadyEvents> ready_events_{};
    };

    Reactor::Impl::Impl() :
        epoll_fd_( ::epoll_create1( EPOLL_CLOEXEC ) ),
        wake_fd_( ::eventfd( emptyEventfdValue,
                             eventfd_flags() ) )
    {
        if( epoll_fd_ == posixFailure )
        {
            startup_error_ = posix_error( "epoll_create1", errno );
            return;
        }

        if( wake_fd_ == posixFailure )
        {
            startup_error_ = posix_error( "eventfd", errno );
            return;
        }

        epoll_event event{};
        event.events   = EPOLLIN;
        event.data.u64 = wakeToken;
        if( ::epoll_ctl( epoll_fd_, EPOLL_CTL_ADD, wake_fd_, &event ) == posixFailure )
        {
            startup_error_ = posix_error( "epoll_ctl wake add", errno );
            return;
        }

        // The deadline source. `epoll_wait`'s timeout is milliseconds, so a
        // 100 us delay used to become 1 ms: a nanosecond API whose
        // implementation threw the nanoseconds away. A timerfd in the same
        // epoll set carries the deadline at nanosecond resolution instead.
        timer_fd_ = ::timerfd_create( CLOCK_MONOTONIC, timerfd_flags() );
        if( timer_fd_ == posixFailure )
        {
            // NOT a startup error: the reactor stays fully functional on the
            // millisecond fallback, and failing construction here would take
            // the overlay's present loop down with it. Precision degrades;
            // nothing stops.
            const int error_number = errno;
            timer_fd_              = invalidFd;
            log::nominal(
                [error_number]( auto& record )
                {
                    record.tag( log::tags::reactor )
                        .value( "timerfd_create", "failed" )
                        .value( "errno", error_number )
                        .value( "fallback", "epoll_timeout_ms" );
                }
            );
            return;
        }

        epoll_event timer_event{};
        timer_event.events   = EPOLLIN;
        timer_event.data.u64 = timerToken;
        if( ::epoll_ctl( epoll_fd_, EPOLL_CTL_ADD, timer_fd_, &timer_event ) ==
            posixFailure )
        {
            const int error_number = errno;
            log::nominal(
                [error_number]( auto& record )
                {
                    record.tag( log::tags::reactor )
                        .value( "epoll_ctl", "timer add failed" )
                        .value( "errno", error_number )
                        .value( "fallback", "epoll_timeout_ms" );
                }
            );
            const auto close_result = ::close( timer_fd_ );
            static_cast<void>( close_result );
            timer_fd_ = invalidFd;
        }
    }

    Reactor::Impl::~Impl() noexcept
    {
        stop();
        if( timer_fd_ != invalidFd )
        {
            const auto close_result = ::close( timer_fd_ );
            static_cast<void>( close_result );
            timer_fd_ = invalidFd;
        }
        if( wake_fd_ != invalidFd )
        {
            const auto close_result = ::close( wake_fd_ );
            static_cast<void>( close_result );
            wake_fd_ = invalidFd;
        }
        if( epoll_fd_ != invalidFd )
        {
            const auto close_result = ::close( epoll_fd_ );
            static_cast<void>( close_result );
            epoll_fd_ = invalidFd;
        }
    }

    grab::Result<void>
    Reactor::Impl::run()
    {
        if( startup_error_.has_value() )
        {
            return std::unexpected( *startup_error_ );
        }

        try
        {
            while( true )
            {
                if( auto result = drain_pending_ops(); !result.has_value() )
                {
                    return result;
                }
                if( stop_requested_.load( std::memory_order_acquire ) )
                {
                    return {};
                }

                dispatch_expired_timers();
                if( stop_requested_.load( std::memory_order_acquire ) )
                {
                    return {};
                }

                const int timeout = wait_timeout();
                const int ready_count =
                    ::epoll_wait( epoll_fd_,
                                  ready_events_.data(),
                                  static_cast<int>( ready_events_.size() ),
                                  timeout );
                if( ready_count == posixFailure )
                {
                    const int error_number = errno;
                    if( error_number == EINTR )
                    {
                        continue;
                    }
                    return std::unexpected( posix_error( "epoll_wait", error_number ) );
                }

                for( int index = firstReadyIndex; index < ready_count; ++index )
                {
                    dispatch_ready_event(
                        ready_events_.at( static_cast<std::size_t>( index ) )
                    );
                }
            }
        }
        catch( const std::exception& exception )
        {
            return std::unexpected( callback_error( "reactor callback", exception ) );
        }
        catch( ... )
        {
            return std::unexpected(
                message_error( "reactor callback: unknown exception" )
            );
        }
    }

    void
    Reactor::Impl::stop() noexcept
    {
        bool expected = false;
        if( stop_requested_.compare_exchange_strong( expected,
                                                     true,
                                                     std::memory_order_acq_rel ) )
        {
            wake();
        }
    }

    std::uint64_t
    Reactor::Impl::add_fd( int                                  fd,
                           std::uint32_t                        events,
                           std::function<void( std::uint32_t )> cb )
    {
        const auto token = next_token_.fetch_add( tokenStep, std::memory_order_relaxed );
        enqueue( PendingOp{
            .kind          = PendingKind::AddFd,
            .token         = token,
            .fd            = fd,
            .events        = events,
            .delay         = {},
            .fd_callback   = std::move( cb ),
            .void_callback = {},
        } );
        return token;
    }

    void
    Reactor::Impl::remove_fd( std::uint64_t token )
    {
        enqueue( PendingOp{
            .kind          = PendingKind::RemoveFd,
            .token         = token,
            .fd            = invalidFd,
            .events        = noFdEvents,
            .delay         = {},
            .fd_callback   = {},
            .void_callback = {},
        } );
    }

    std::uint64_t
    Reactor::Impl::add_timer( std::chrono::nanoseconds delay,
                              std::function<void()>    cb )
    {
        const auto token = next_token_.fetch_add( tokenStep, std::memory_order_relaxed );
        enqueue( PendingOp{
            .kind          = PendingKind::AddTimer,
            .token         = token,
            .fd            = invalidFd,
            .events        = noFdEvents,
            .delay         = delay,
            .fd_callback   = {},
            .void_callback = std::move( cb ),
        } );
        return token;
    }

    void
    Reactor::Impl::post( std::function<void()> fn )
    {
        enqueue( PendingOp{
            .kind          = PendingKind::Task,
            .token         = wakeToken,
            .fd            = invalidFd,
            .events        = noFdEvents,
            .delay         = {},
            .fd_callback   = {},
            .void_callback = std::move( fn ),
        } );
    }

    void
    Reactor::Impl::enqueue( PendingOp op )
    {
        {
            const std::scoped_lock lock( mutex_ );
            pending_ops_.push_back( std::move( op ) );
        }
        wake();
    }

    void
    Reactor::Impl::wake() const noexcept
    {
        if( wake_fd_ == invalidFd )
        {
            return;
        }

        while( true )
        {
            if( ::eventfd_write( wake_fd_, wakeEventfdValue ) == posixSuccess )
            {
                return;
            }
            const int error_number = errno;
            if( error_number == EINTR )
            {
                continue;
            }
            return;
        }
    }

    void
    Reactor::Impl::drain_wake_fd() const noexcept
    {
        while( true )
        {
            eventfd_t value = emptyEventfdValue;
            if( ::eventfd_read( wake_fd_, &value ) == posixSuccess )
            {
                continue;
            }

            const int error_number = errno;
            if( error_number == EINTR )
            {
                continue;
            }
            return;
        }
    }

    grab::Result<void>
    Reactor::Impl::drain_pending_ops()
    {
        std::vector<PendingOp> ops;
        {
            const std::scoped_lock lock( mutex_ );
            ops.swap( pending_ops_ );
        }

        for( auto& op : ops )
        {
            switch( op.kind )
            {
                case PendingKind::AddFd :
                    {
                        // A client may close its fd (e.g. an immediate stop
                        // after start) between enqueueing the registration
                        // and this drain. That loses the one registration —
                        // never the shared loop: every other client's fence
                        // and fd would silently hang if run() exited here.
                        static_cast<void>( add_fd_on_reactor( op ) );
                        break;
                    }
                case PendingKind::RemoveFd :
                    {
                        // Same containment: a failed deregistration affects
                        // only that token.
                        static_cast<void>( remove_fd_on_reactor( op.token ) );
                        break;
                    }
                case PendingKind::AddTimer :
                    add_timer_on_reactor( std::move( op ) );
                    break;
                case PendingKind::Task :
                    op.void_callback();
                    break;
            }
        }
        return {};
    }

    grab::Result<void>
    Reactor::Impl::add_fd_on_reactor( PendingOp& op )
    {
        if( consume_cancelled_token( op.token ) )
        {
            return {};
        }

        epoll_event event{};
        event.events   = op.events;
        event.data.u64 = op.token;
        if( ::epoll_ctl( epoll_fd_, EPOLL_CTL_ADD, op.fd, &event ) == posixFailure )
        {
            return std::unexpected( posix_error( "epoll_ctl add", errno ) );
        }
        fds_.push_back( FdRegistration{
            .token    = op.token,
            .fd       = op.fd,
            .callback = std::move( op.fd_callback ),
        } );
        return {};
    }

    grab::Result<void>
    Reactor::Impl::remove_fd_on_reactor( std::uint64_t token )
    {
        const auto registration =
            std::ranges::find_if( fds_,
                                  [token]( const FdRegistration& candidate )
                                  {
                                      return candidate.token == token;
                                  } );
        if( registration == fds_.end() )
        {
            cancelled_tokens_.push_back( token );
            return {};
        }

        if( ::epoll_ctl( epoll_fd_, EPOLL_CTL_DEL, registration->fd, nullptr ) ==
            posixFailure )
        {
            const int error_number = errno;
            if( error_number != EBADF && error_number != ENOENT )
            {
                return std::unexpected( posix_error( "epoll_ctl del", error_number ) );
            }
        }
        fds_.erase( registration );
        return {};
    }

    void
    Reactor::Impl::add_timer_on_reactor( PendingOp op )
    {
        // The deadline is RELATIVE: `now()` as observed on this drain, plus
        // the delay. A caller that rearms from inside its own callback
        // therefore re-bases on however late the previous firing was, and
        // that overshoot compounds across a chain. The absolute timerfd arm
        // does not fix this and was never meant to — it removes the
        // ceil-to-millisecond and the per-arm rounding, nothing more. An
        // interval that must not drift needs an absolute deadline in the API,
        // which `add_timer` does not offer.
        timers_.push_back( TimerRegistration{
            .token    = op.token,
            .deadline = std::chrono::steady_clock::now() + op.delay,
            .callback = std::move( op.void_callback ),
        } );
        std::ranges::push_heap( timers_, TimerLater{} );
    }

    void
    Reactor::Impl::dispatch_expired_timers()
    {
        const auto now = std::chrono::steady_clock::now();
        while( !timers_.empty() && timers_.front().deadline <= now )
        {
            std::ranges::pop_heap( timers_, TimerLater{} );
            auto timer = std::move( timers_.back() );
            timers_.pop_back();
            timer.callback();
            if( stop_requested_.load( std::memory_order_acquire ) )
            {
                return;
            }
        }
    }

    void
    Reactor::Impl::dispatch_ready_event( const epoll_event& event )
    {
        if( event.data.u64 == wakeToken )
        {
            drain_wake_fd();
            return;
        }

        if( event.data.u64 == timerToken )
        {
            // Level-triggered: an undrained expiry would make every
            // subsequent `epoll_wait` return instantly. The timers it stands
            // for are fired by `dispatch_expired_timers()` at the top of the
            // next iteration, not here — the heap, not the fd, decides which
            // callbacks are due.
            drain_timer_fd();
            return;
        }

        const auto registration =
            std::ranges::find_if( fds_,
                                  [&event]( const FdRegistration& candidate )
                                  {
                                      return candidate.token == event.data.u64;
                                  } );
        if( registration == fds_.end() )
        {
            return;
        }
        registration->callback( event.events );
    }

    bool
    Reactor::Impl::arm_timer_fd() const
    {
        // An all-zero `it_value` disarms, which is exactly right with no
        // timer pending: the only remaining wakeups are fd readiness and the
        // wake eventfd, both of which epoll reports on their own.
        ::itimerspec spec{};
        if( !timers_.empty() )
        {
            // TFD_TIMER_ABSTIME wants a CLOCK_MONOTONIC instant. On
            // glibc/libstdc++ `steady_clock` IS CLOCK_MONOTONIC — same epoch,
            // same tick — so a `steady_clock::time_point`'s
            // `time_since_epoch()` is already expressed in the timer's own
            // time base and needs no offset. That identity is what makes an
            // absolute arm correct here; it is not universal, and a port to
            // another platform has to re-establish it before reusing this.
            //
            // `timers_` is a binary heap ordered by `TimerLater`, so
            // `front()` is the *earliest* deadline: arming from it is
            // arming from the next thing due.
            const auto since_epoch = timers_.front().deadline.time_since_epoch();
            const auto whole_seconds =
                std::chrono::duration_cast<std::chrono::seconds>( since_epoch );
            const auto remainder =
                std::chrono::duration_cast<std::chrono::nanoseconds>( since_epoch -
                                                                      whole_seconds );

            spec.it_value.tv_sec =
                static_cast<decltype( spec.it_value.tv_sec )>( whole_seconds.count() );
            spec.it_value.tv_nsec =
                static_cast<decltype( spec.it_value.tv_nsec )>( remainder.count() );
            if( spec.it_value.tv_sec == 0 && spec.it_value.tv_nsec == 0 )
            {
                spec.it_value.tv_nsec = leastArmedNanoseconds;
            }
        }

        // A deadline already in the past is not an error: an absolute arm in
        // the past expires immediately, which is what a missed deadline
        // should do.
        if( ::timerfd_settime( timer_fd_, TFD_TIMER_ABSTIME, &spec, nullptr ) ==
            posixFailure )
        {
            const int error_number = errno;
            log::nominal(
                [error_number]( auto& record )
                {
                    record.tag( log::tags::reactor )
                        .value( "timerfd_settime", "failed" )
                        .value( "errno", error_number )
                        .value( "fallback", "epoll_timeout_ms" );
                }
            );
            return false;
        }
        return true;
    }

    void
    Reactor::Impl::drain_timer_fd() const noexcept
    {
        if( timer_fd_ == invalidFd )
        {
            return;
        }

        while( true )
        {
            std::uint64_t expirations = noExpirations;
            const ssize_t bytes =
                ::read( timer_fd_, &expirations, sizeof( expirations ) );
            if( bytes > noBytesRead )
            {
                continue;
            }

            const int error_number = errno;
            if( bytes == posixFailure && error_number == EINTR )
            {
                continue;
            }
            return;
        }
    }

    int
    Reactor::Impl::wait_timeout() const
    {
        // Without a working timerfd the loop MUST keep computing a finite
        // timeout. Waiting forever with nothing armed is a deadlock, not a
        // degraded mode: every `add_timer` client — the overlay's 60 FPS
        // present loop included — would hang behind it.
        if( timer_fd_ == invalidFd )
        {
            return epoll_timeout();
        }
        if( !arm_timer_fd() )
        {
            return epoll_timeout();
        }
        return infiniteWait;
    }

    int
    Reactor::Impl::epoll_timeout() const
    {
        if( timers_.empty() )
        {
            return infiniteWait;
        }

        const auto now      = std::chrono::steady_clock::now();
        const auto deadline = timers_.front().deadline;
        if( deadline <= now )
        {
            return noWait;
        }

        const auto remaining = deadline - now;
        const auto millis    = std::chrono::ceil<std::chrono::milliseconds>( remaining );
        const auto capped =
            std::min<std::chrono::milliseconds::rep>( millis.count(),
                                                      std::numeric_limits<int>::max() );
        return static_cast<int>( capped );
    }

    bool
    Reactor::Impl::consume_cancelled_token( std::uint64_t token )
    {
        const auto cancelled = std::ranges::find( cancelled_tokens_, token );
        if( cancelled == cancelled_tokens_.end() )
        {
            return false;
        }
        cancelled_tokens_.erase( cancelled );
        return true;
    }

    Reactor::Reactor() :
        impl_( std::make_unique<Impl>() )
    {
    }

    Reactor::~Reactor() = default;

    grab::Result<void>
    Reactor::run()
    {
        return impl_->run();
    }

    void
    Reactor::stop() noexcept
    {
        impl_->stop();
    }

    std::uint64_t
    Reactor::add_fd( int                                  fd,
                     std::uint32_t                        events,
                     std::function<void( std::uint32_t )> cb )
    {
        return impl_->add_fd( fd, events, std::move( cb ) );
    }

    void
    Reactor::remove_fd( std::uint64_t token )
    {
        impl_->remove_fd( token );
    }

    std::uint64_t
    Reactor::add_timer( std::chrono::nanoseconds delay,
                        std::function<void()>    cb )
    {
        return impl_->add_timer( delay, std::move( cb ) );
    }

    void
    Reactor::post( std::function<void()> fn )
    {
        impl_->post( std::move( fn ) );
    }

}    // namespace grab::core
