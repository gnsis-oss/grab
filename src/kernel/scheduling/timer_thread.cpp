#include "kernel/scheduling/timer_thread.hpp"

#include <chrono>
#include <memory>
#include <vector>

namespace grab::kernel::scheduling
{

    // PHASE 0 STUB. No thread is started and no fd is created, so nothing here
    // can wake anything; every deadline is reported as never due and wake_fd()
    // is invalid. The timer-thread unit replaces this file wholesale.
    class TimerThread::Impl final
    {
        public:

            Token next_token_{ 1U };
    };

    TimerThread::TimerThread() :
        impl_( std::make_unique<Impl>() )
    {
    }

    TimerThread::~TimerThread() = default;

    TimerThread::Token
    TimerThread::arm( std::chrono::steady_clock::time_point deadline )
    {
        ( void )deadline;
        const auto token = impl_->next_token_;
        ++impl_->next_token_;
        return token;
    }

    void
    TimerThread::cancel( Token token )
    {
        ( void )token;
    }

    int
    TimerThread::wake_fd() const noexcept
    {
        return -1;
    }

    std::vector<TimerThread::Token>
    TimerThread::drain()
    {
        return {};
    }

    void
    TimerThread::stop() noexcept
    {
    }

}    // namespace grab::kernel::scheduling
