#include "session/poll_wait.hpp"

#include <chrono>
#include <functional>
#include <thread>

namespace grab::session
{

    Probe
    poll_until( const std::function<Probe()>& probe,
                std::chrono::milliseconds     timeout,
                std::chrono::milliseconds     interval )
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for( ;; )
        {
            const Probe answer = probe();
            if( answer != Probe::Retry )
            {
                return answer;
            }
            // Checked after the probe, so a zero timeout still asks once: a
            // caller polling an already-satisfied precondition should not have
            // to pay for a sleep to hear so.
            if( std::chrono::steady_clock::now() >= deadline )
            {
                return Probe::Retry;
            }
            std::this_thread::sleep_for( interval );
        }
    }

}    // namespace grab::session
