#include "kernel/scheduling/timer_thread.hpp"
#include "kernel/support/diag.hpp"
#include "kernel/support/log.hpp"
#include "kernel/support/log_tags.hpp"
#include "kernel/support/step_diag.hpp"

// clang-format off
#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <poll.h>
#include <string_view>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>
// clang-format on

namespace grab::kernel::scheduling
{
    namespace
    {

        constexpr int                invalidFd            = -1;
        constexpr int                posixFailure         = -1;
        constexpr int                posixSuccess         = 0;
        constexpr int                infiniteWait         = -1;
        constexpr int                noError              = 0;

        constexpr std::size_t        noEntries            = 0U;
        constexpr std::size_t        pollFdCount          = 2U;
        constexpr std::size_t        timerPollIndex       = 0U;
        constexpr std::size_t        controlPollIndex     = 1U;

        constexpr TimerThread::Token firstToken           = 1U;
        constexpr TimerThread::Token tokenStep            = 1U;

        constexpr eventfd_t          emptyEventfdValue    = 0U;
        constexpr eventfd_t          wakeEventfdValue     = 1U;

        constexpr std::int64_t       nanosecondsPerSecond = 1'000'000'000;

        // What it costs to time a call is two clock reads plus a lock
        // acquisition, so the per-call tallies are gated and vanish below
        // Verbose. Wake latency and the counters are NOT gated: run() already
        // reads the clock to decide what expired and already holds the lock
        // over the vectors being measured, so they are obtained for nothing.
        constexpr grab::log::Level   callTimingLevel = grab::log::Level::Verbose;
        constexpr bool         callTimingEnabled = grab::log::enabled( callTimingLevel );

        // An itimerspec whose it_value is {0,0} DISARMS the timer, so a
        // deadline at or before the CLOCK_MONOTONIC epoch must be nudged to the
        // smallest representable positive absolute time instead of being
        // silently turned into "never".
        constexpr std::int64_t earliestArmableNanoseconds = 1;

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

        // steady_clock's epoch is CLOCK_MONOTONIC's on glibc/libstdc++, so the
        // raw time_since_epoch() count already IS a CLOCK_MONOTONIC absolute
        // time and TFD_TIMER_ABSTIME can consume it without a conversion
        // through any other clock. This is a Linux/glibc assumption; on a
        // platform where the two epochs differ this function is where the
        // offset would have to be applied.
        [[nodiscard]]
        ::itimerspec
        absolute_spec( std::chrono::steady_clock::time_point deadline ) noexcept
        {
            auto since_epoch = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   deadline.time_since_epoch()
            )
                                   .count();
            if( since_epoch < earliestArmableNanoseconds )
            {
                since_epoch = earliestArmableNanoseconds;
            }

            ::itimerspec spec{};
            spec.it_value.tv_sec =
                static_cast<decltype( spec.it_value.tv_sec )>( since_epoch /
                                                               nanosecondsPerSecond );
            spec.it_value.tv_nsec =
                static_cast<decltype( spec.it_value.tv_nsec )>( since_epoch %
                                                                nanosecondsPerSecond );
            return spec;
        }

        void
        close_fd( int& descriptor ) noexcept
        {
            if( descriptor == invalidFd )
            {
                return;
            }
            const int result = ::close( descriptor );
            static_cast<void>( result );
            descriptor = invalidFd;
        }

    }    // namespace

    class TimerThread::Impl final
    {
        public:

            Impl();
            ~Impl() noexcept;

            Impl( const Impl& ) = delete;
            Impl&
            operator=( const Impl& ) = delete;
            Impl( Impl&& ) noexcept  = delete;
            Impl&
            operator=( Impl&& ) noexcept = delete;

            [[nodiscard]]
            Token
            arm( std::chrono::steady_clock::time_point deadline );

            void
            cancel( Token token );

            [[nodiscard]]
            int
            wake_fd() const noexcept;

            [[nodiscard]]
            std::vector<Token>
            drain();

            void
            stop() noexcept;

            void
            snapshot_instrument( grab::diag::Instrument& into ) const noexcept;

            [[nodiscard]]
            ScheduleCounters
            counters() const noexcept;

            [[nodiscard]]
            std::chrono::nanoseconds
            worst_wake_latency() const noexcept;

        private:

            struct Entry
            {
                    Token                                 token{};
                    std::chrono::steady_clock::time_point deadline{};
            };

            // Records a per-call duration, taking the ONE mutex to do it.
            // Compiled out below callTimingLevel, lock included. Never called
            // with mutex_ already held -- the whole point of doing it here
            // rather than through a diag::Measure at function scope is that a
            // Measure would record on destruction, which is after the caller's
            // scoped_lock has already been released.
            void
            record_call( std::string_view         name,
                         std::chrono::nanoseconds elapsed ) noexcept
            {
                if constexpr( callTimingEnabled )
                {
                    const std::scoped_lock lock{ mutex_ };
                    instrument_.record( name, elapsed );
                }
                else
                {
                    static_cast<void>( name );
                    static_cast<void>( elapsed );
                }
            }

            // The internal thread body. It waits, it moves expired tokens into
            // due_, and it makes wake_fd_ readable. It calls nothing the caller
            // supplied, because there is nothing the caller can supply.
            void
            run();

            // Moves every entry whose deadline has passed into due_ and makes
            // wake_fd_ readable, then reports the earliest remaining deadline.
            // Called with mutex_ held; the eventfd write happens under the same
            // lock drain() takes, which is what makes "tokens taken" and
            // "readability cleared" one atomic step.
            [[nodiscard]]
            std::optional<std::chrono::steady_clock::time_point>
            collect_due_locked( std::chrono::steady_clock::time_point now );

            void
            arm_timer(
                std::optional<std::chrono::steady_clock::time_point> deadline
            ) const noexcept;

            void
            signal_fd( int descriptor ) const noexcept;

            void
            drain_eventfd( int descriptor ) const noexcept;

            void
                                   drain_timer_fd() const noexcept;

            // One mutex, and only one: every piece of shared state below is
            // under it. A second lock is what produced grab's only real TSan
            // finding (the EventBus / StateSnapshotProvider inversion), so
            // there is no second lock to order against.
            mutable std::mutex     mutex_;

            // The frontier is 2–10 timers wide (design §4.8), so a flat vector
            // with a linear scan beats a heap and keeps cancel() O(n) without a
            // side index.
            std::vector<Entry>     armed_;
            std::vector<Token>     due_;
            Token                  next_token_ = firstToken;
            bool                   stopping_   = false;

            // Everything the class knows about its own accuracy, under the
            // same mutex as the state it is measured from -- so a snapshot is
            // never half a run behind the queue it describes.
            grab::diag::Instrument instrument_{};
            ScheduleCounters       counters_{};

            // The deadline the timerfd is currently programmed for, as last
            // decided by collect_due_locked. Kept because "did this arm() beat
            // the wait already in flight" is otherwise an O(n) rescan of
            // armed_ on the hot path, and because the answer must be about
            // what the TIMER is set to rather than about what is merely queued.
            std::optional<std::chrono::steady_clock::time_point> timer_armed_for_{};

            // stop() is idempotent and the destructor calls it again, so the
            // summary needs its own latch or a run reports itself twice.
            bool                                                 summarised_ = false;

            // Set once during construction, before worker_ exists, and read
            // unsynchronised afterwards: thread creation is the happens-before
            // edge. Closed only after the join in the destructor.
            int         wake_fd_    = invalidFd;    // readable when due_ is non-empty
            int         control_fd_ = invalidFd;    // "re-evaluate" / "stop"
            int         timer_fd_   = invalidFd;    // the earliest armed deadline

            std::thread worker_;
    };

    TimerThread::Impl::Impl()
    {
        // errno is captured at each call rather than once at the end: a later
        // successful call may leave errno set to something unrelated, and a
        // startup diagnostic that names the wrong reason is worse than none.
        int error_number = noError;

        wake_fd_         = ::eventfd( emptyEventfdValue, eventfd_flags() );
        if( wake_fd_ == invalidFd && error_number == noError )
        {
            error_number = errno;
        }

        control_fd_ = ::eventfd( emptyEventfdValue, eventfd_flags() );
        if( control_fd_ == invalidFd && error_number == noError )
        {
            error_number = errno;
        }

        timer_fd_ = ::timerfd_create( CLOCK_MONOTONIC, timerfd_flags() );
        if( timer_fd_ == invalidFd && error_number == noError )
        {
            error_number = errno;
        }

        if( wake_fd_ == invalidFd || control_fd_ == invalidFd || timer_fd_ == invalidFd )
        {
            stopping_ = true;
            grab::log::nominal(
                [&]( auto& event )
                {
                    event.tag( grab::log::tags::timer )
                        .value( "op", "start" )
                        .value( "error", "descriptor allocation failed" )
                        .value( "errno", error_number );
                }
            );
            return;
        }

        try
        {
            worker_ = std::thread(
                [this]
                {
                    run();
                }
            );
        }
        catch( ... )
        {
            // A constructor with no Result cannot report this. Degrade to an
            // inert timer set rather than throwing out of a pimpl allocation:
            // arm() keeps handing out tokens, none of them ever expire, and
            // the record below is the only witness.
            stopping_ = true;
            grab::log::nominal(
                [&]( auto& event )
                {
                    event.tag( grab::log::tags::timer )
                        .value( "op", "start" )
                        .value( "error", "timer thread could not be created" );
                }
            );
        }
    }

    TimerThread::Impl::~Impl() noexcept
    {
        stop();
        close_fd( timer_fd_ );
        close_fd( control_fd_ );
        close_fd( wake_fd_ );
    }

    TimerThread::Token
    TimerThread::Impl::arm( std::chrono::steady_clock::time_point deadline )
    {
        const grab::diag::Scope<callTimingLevel> timing;

        Token                                    token  = firstToken;
        std::size_t                              depth  = noEntries;
        bool                                     nearer = false;
        {
            const std::scoped_lock lock{ mutex_ };
            token        = next_token_;
            next_token_ += tokenStep;
            if( stopping_ )
            {
                return token;
            }
            armed_.push_back( Entry{ .token = token, .deadline = deadline } );

            // Measured here rather than after the lock is dropped: a depth
            // read outside it would be a different queue than the one this
            // arm() joined.
            depth  = armed_.size();
            nearer = timer_armed_for_.has_value() && deadline < *timer_armed_for_;

            ++counters_.arms;
            counters_.armedDepthTotal += static_cast<std::uint64_t>( depth );
            counters_.deepestArmed     = std::max( counters_.deepestArmed, depth );
            if( nearer )
            {
                ++counters_.nearerRearms;
            }
        }

        // Level-triggered and written after the state change, so a thread
        // already inside poll() returns immediately and recomputes: the
        // classic self-pipe ordering, with no window where a nearer deadline
        // is armed but not waited on.
        signal_fd( control_fd_ );

        grab::log::verbose(
            [&]( auto& event )
            {
                event.tag( grab::log::tags::timer )
                    .value( "op", "arm" )
                    .value( "token", token )
                    .value( "deadline_ns",
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                deadline.time_since_epoch()
                            )
                                .count() )
                    .value( "armed", depth )
                    .value( "nearer", nearer );
            }
        );
        record_call( timer_tally::arm, timing.elapsed() );
        return token;
    }

    void
    TimerThread::Impl::cancel( Token token )
    {
        const grab::diag::Scope<callTimingLevel> timing;

        std::size_t                              disarmed    = noEntries;
        std::size_t                              undelivered = noEntries;
        {
            const std::scoped_lock lock{ mutex_ };
            ++counters_.cancels;
            disarmed = std::erase_if( armed_,
                                      [token]( const Entry& entry )
                                      {
                                          return entry.token == token;
                                      } );

            // Also dropped if it had already expired but not yet been drained,
            // so "after cancel() returns, the token never appears again" holds
            // without the caller having to reason about the race.
            undelivered = std::erase( due_, token );
        }
        signal_fd( control_fd_ );

        grab::log::verbose(
            [&]( auto& event )
            {
                event.tag( grab::log::tags::timer )
                    .value( "op", "cancel" )
                    .value( "token", token )
                    .value( "disarmed", disarmed )
                    .value( "undelivered", undelivered );
            }
        );
        record_call( timer_tally::cancel, timing.elapsed() );
    }

    int
    TimerThread::Impl::wake_fd() const noexcept
    {
        return wake_fd_;
    }

    std::vector<TimerThread::Token>
    TimerThread::Impl::drain()
    {
        const grab::diag::Scope<callTimingLevel> timing;

        std::vector<Token>                       taken;
        {
            const std::scoped_lock lock{ mutex_ };

            const std::size_t      depth = due_.size();
            ++counters_.drains;
            counters_.dueDepthTotal += static_cast<std::uint64_t>( depth );
            counters_.deepestDue     = std::max( counters_.deepestDue, depth );
            if( depth == noEntries )
            {
                // A wake that delivered nothing. The syscall, the lock and the
                // round trip were spent either way, so this is the honest
                // count of what the pacing loop paid for nothing.
                ++counters_.spuriousDrains;
            }

            taken.swap( due_ );
            drain_eventfd( wake_fd_ );

            if constexpr( callTimingEnabled )
            {
                // Recorded inside the lock rather than through record_call:
                // the lock is already held, and re-taking it would price the
                // second acquisition into the first one's measurement.
                instrument_.record( timer_tally::drain, timing.elapsed() );
            }
        }
        return taken;
    }

    void
    TimerThread::Impl::stop() noexcept
    {
        {
            const std::scoped_lock lock{ mutex_ };
            stopping_ = true;
        }
        signal_fd( control_fd_ );

        if( worker_.joinable() )
        {
            try
            {
                worker_.join();
            }
            catch( ... )
            {
                // Nothing left to do at teardown; noexcept is the contract.
            }
        }

        // The one line a human gets for free, without asking for --trace or
        // reading a single verbose record: did the deadlines hold. Emitted
        // once -- stop() is idempotent and the destructor calls it again.
        ScheduleCounters totals{};
        auto             worst = std::chrono::nanoseconds::zero();
        auto             mean  = std::chrono::nanoseconds::zero();
        {
            const std::scoped_lock lock{ mutex_ };
            if( summarised_ )
            {
                return;
            }
            summarised_ = true;
            totals      = counters_;
            for( const auto& tally : instrument_.tallies() )
            {
                if( tally.name == timer_tally::wakeLatency )
                {
                    worst = tally.longest;
                    mean  = tally.mean();
                }
            }
        }

        grab::log::nominal(
            [&totals, worst, mean]( auto& event )
            {
                event.tag( grab::log::tags::timer )
                    .value( "op", "summary" )
                    .value( "arms", totals.arms )
                    .value( "fires", totals.fires )
                    .value( "cancels", totals.cancels )
                    .value( "drains", totals.drains )
                    .value( "spurious", totals.spuriousDrains )
                    .value( "rearms", totals.nearerRearms )
                    .value( "deepest_armed", totals.deepestArmed )
                    .value( "worst_wake_ns", worst.count() )
                    .value( "mean_wake_ns", mean.count() );
            }
        );
    }

    void
    TimerThread::Impl::snapshot_instrument( grab::diag::Instrument& into ) const noexcept
    {
        const std::scoped_lock lock{ mutex_ };
        into = instrument_;
    }

    ScheduleCounters
    TimerThread::Impl::counters() const noexcept
    {
        const std::scoped_lock lock{ mutex_ };
        return counters_;
    }

    std::chrono::nanoseconds
    TimerThread::Impl::worst_wake_latency() const noexcept
    {
        const std::scoped_lock lock{ mutex_ };
        for( const auto& tally : instrument_.tallies() )
        {
            if( tally.name == timer_tally::wakeLatency )
            {
                return tally.longest;
            }
        }
        return std::chrono::nanoseconds::zero();
    }

    void
    TimerThread::Impl::run()
    {
        std::array<::pollfd, pollFdCount> descriptors{};
        descriptors[timerPollIndex].fd       = timer_fd_;
        descriptors[timerPollIndex].events   = static_cast<short>( POLLIN );
        descriptors[controlPollIndex].fd     = control_fd_;
        descriptors[controlPollIndex].events = static_cast<short>( POLLIN );

        while( true )
        {
            std::optional<std::chrono::steady_clock::time_point> next;
            {
                const std::scoped_lock lock{ mutex_ };
                if( stopping_ )
                {
                    return;
                }
                next = collect_due_locked( std::chrono::steady_clock::now() );
            }

            // Re-arming also clears any expiration the previous wait left
            // pending on timer_fd_, so a stale readability cannot spin the
            // loop.
            arm_timer( next );

            const int ready = ::poll( descriptors.data(),
                                      static_cast<::nfds_t>( pollFdCount ),
                                      infiniteWait );
            if( ready == posixFailure )
            {
                const int error_number = errno;
                if( error_number == EINTR )
                {
                    continue;
                }
                grab::log::nominal(
                    [&]( auto& event )
                    {
                        event.tag( grab::log::tags::timer )
                            .value( "op", "poll" )
                            .value( "errno", error_number );
                    }
                );
                return;
            }

            if( ( descriptors[timerPollIndex].revents & POLLIN ) != 0 )
            {
                drain_timer_fd();
            }
            if( ( descriptors[controlPollIndex].revents & POLLIN ) != 0 )
            {
                drain_eventfd( control_fd_ );
            }
        }
    }

    std::optional<std::chrono::steady_clock::time_point>
    TimerThread::Impl::collect_due_locked( std::chrono::steady_clock::time_point now )
    {
        // WAKE LATENCY IS MEASURED HERE, AND COSTS NOTHING. `now` was read
        // once by run() to decide what expired at all, so the gap between a
        // token's requested deadline and the instant this thread noticed it
        // had passed is a subtraction of two values already in hand -- no
        // extra clock read on the hot path, which is why it is unconditional.
        //
        // It cannot be negative: the predicate only collects entries whose
        // deadline is at or before `now`.
        //
        // erase_if applies the predicate exactly once per element, in order,
        // which is what makes collecting the expired tokens inside it correct
        // rather than merely convenient.
        auto       worst = std::chrono::nanoseconds::zero();
        const auto expired =
            std::erase_if( armed_,
                           [&]( const Entry& entry )
                           {
                               if( entry.deadline > now )
                               {
                                   return false;
                               }
                               const auto latency =
                                   std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       now - entry.deadline
                                   );
                               instrument_.record( timer_tally::wakeLatency, latency );
                               worst = std::max( worst, latency );
                               ++counters_.fires;

                               grab::log::verbose(
                                   [&entry, latency]( auto& event )
                                   {
                                       event.tag( grab::log::tags::timer )
                                           .value( "op", "fire" )
                                           .value( "token", entry.token )
                                           .value( "latency_ns", latency.count() );
                                   }
                               );

                               due_.push_back( entry.token );
                               return true;
                           } );

        if( expired > noEntries )
        {
            signal_fd( wake_fd_ );
            grab::log::verbose(
                [&]( auto& event )
                {
                    event.tag( grab::log::tags::timer )
                        .value( "op", "expire" )
                        .value( "count", expired )
                        .value( "armed", armed_.size() )
                        .value( "worst_ns", worst.count() );
                }
            );
        }

        const auto earliest = std::ranges::min_element( armed_, {}, &Entry::deadline );
        if( earliest == armed_.end() )
        {
            timer_armed_for_.reset();
            return std::nullopt;
        }
        // What the caller is about to program the timerfd with, which is the
        // only thing a later arm() can meaningfully be "nearer" than.
        timer_armed_for_ = earliest->deadline;
        return earliest->deadline;
    }

    void
    TimerThread::Impl::arm_timer(
        std::optional<std::chrono::steady_clock::time_point> deadline
    ) const noexcept
    {
        ::itimerspec spec{};    // all-zero it_value disarms
        if( deadline.has_value() )
        {
            spec = absolute_spec( *deadline );
        }

        if( ::timerfd_settime( timer_fd_, TFD_TIMER_ABSTIME, &spec, nullptr ) ==
            posixFailure )
        {
            const int error_number = errno;
            grab::log::nominal(
                [&]( auto& event )
                {
                    event.tag( grab::log::tags::timer )
                        .value( "op", "settime" )
                        .value( "errno", error_number );
                }
            );
        }
    }

    void
    TimerThread::Impl::signal_fd( int descriptor ) const noexcept
    {
        if( descriptor == invalidFd )
        {
            return;
        }

        while( true )
        {
            if( ::eventfd_write( descriptor, wakeEventfdValue ) == posixSuccess )
            {
                return;
            }
            if( errno == EINTR )
            {
                continue;
            }
            return;
        }
    }

    void
    TimerThread::Impl::drain_eventfd( int descriptor ) const noexcept
    {
        if( descriptor == invalidFd )
        {
            return;
        }

        while( true )
        {
            eventfd_t value = emptyEventfdValue;
            if( ::eventfd_read( descriptor, &value ) == posixSuccess )
            {
                continue;
            }
            if( errno == EINTR )
            {
                continue;
            }
            return;
        }
    }

    void
    TimerThread::Impl::drain_timer_fd() const noexcept
    {
        while( true )
        {
            std::uint64_t   expirations = 0U;
            const ::ssize_t read_bytes =
                ::read( timer_fd_, &expirations, sizeof( expirations ) );
            if( read_bytes == static_cast<::ssize_t>( sizeof( expirations ) ) )
            {
                return;
            }
            if( read_bytes == posixFailure && errno == EINTR )
            {
                continue;
            }
            return;
        }
    }

    TimerThread::TimerThread() :
        impl_( std::make_unique<Impl>() )
    {
    }

    TimerThread::~TimerThread() = default;

    TimerThread::Token
    TimerThread::arm( std::chrono::steady_clock::time_point deadline )
    {
        return impl_->arm( deadline );
    }

    void
    TimerThread::cancel( Token token )
    {
        impl_->cancel( token );
    }

    int
    TimerThread::wake_fd() const noexcept
    {
        return impl_->wake_fd();
    }

    std::vector<TimerThread::Token>
    TimerThread::drain()
    {
        return impl_->drain();
    }

    void
    TimerThread::stop() noexcept
    {
        impl_->stop();
    }

    const grab::diag::Instrument&
    TimerThread::instrument() const noexcept
    {
        impl_->snapshot_instrument( snapshot_ );
        return snapshot_;
    }

    ScheduleCounters
    TimerThread::counters() const noexcept
    {
        return impl_->counters();
    }

    std::chrono::nanoseconds
    TimerThread::worst_wake_latency() const noexcept
    {
        return impl_->worst_wake_latency();
    }

}    // namespace grab::kernel::scheduling
