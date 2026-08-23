#include "grab/result.hpp"
#include "grab/sequence_types.hpp"
#include "kernel/scheduling/timer_thread.hpp"
#include "kernel/sequence/drive.hpp"
#include "kernel/sequence/player.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <poll.h>
#include <string>

namespace grab::kernel::sequence
{

    namespace
    {

        using Clock = std::chrono::steady_clock;

        // The loop wakes on the timer's eventfd; this bounds how long it can
        // block if that wake is ever lost, so a stuck run is a slow run rather
        // than a hung process.
        constexpr int  maximumWaitMs = 250;
        constexpr int  minimumWaitMs = 1;

        // What to arm when the player wants a tick but states no deadline --
        // an Opaque body, which finishes when it finishes. Polling it at this
        // cadence is what keeps the loop off a spin.
        constexpr auto opaquePollPeriod = std::chrono::milliseconds{ 1 };

        // poll() rather than a sleep: the wait is owned by the timer thread's
        // eventfd, and the timeout is only a bound on how long a lost wake can
        // cost.
        void
        wait_readable( int                       descriptor,
                       std::chrono::milliseconds remaining )
        {
            auto budget = static_cast<std::int64_t>( remaining.count() );
            budget      = std::max<std::int64_t>( budget, minimumWaitMs );
            budget      = std::min<std::int64_t>( budget, maximumWaitMs );

            pollfd watched{
                .fd      = descriptor,
                .events  = POLLIN,
                .revents = 0,
            };
            while( ::poll( &watched, 1U, static_cast<int>( budget ) ) < 0 )
            {
                if( errno != EINTR )
                {
                    return;
                }
            }
        }

    }    // namespace

    grab::Result<void>
    drive( Player&             player,
           const DriveOptions& options )
    {
        std::optional<grab::kernel::scheduling::TimerThread> timers;

        auto                                                 started = player.play();
        if( !started.has_value() )
        {
            return started;
        }

        grab::Result<void> outcome{};
        while( true )
        {
            auto pumped = player.pump( Clock::now() );
            // Run BEFORE the break checks, so the samples the last pump
            // produced are drawn on the way out rather than left in the queue
            // for a teardown that may not look at it.
            if( options.on_pump )
            {
                options.on_pump();
            }
            if( !pumped.has_value() )
            {
                outcome = pumped;
                break;
            }
            const auto state = player.state();
            if( state !=
                grab::sequence::PlayState::Playing &&
                state != grab::sequence::PlayState::Paused )
            {
                break;
            }
            // A cancellation is an unwind, not a stop: interrupt() exits every
            // entered step in reverse and reaps the holds the completed ones
            // left down, which is the only thing that lifts a pointer capture
            // taken between overlay.grab and an overlay.release that is now
            // never going to run.
            if( options.cancelled && options.cancelled() )
            {
                ( void )player.interrupt();
                outcome = grab::fail( grab::ErrorCode::Cancelled,
                                      std::string{ options.cancel_reason } );
                break;
            }

            const auto now      = Clock::now();
            const auto deadline = player.next_deadline();
            // The frontier is non-empty while Playing, and every member is
            // either Ready or Running, so both of those carry a candidate.
            // Reaching here without one means the run cannot advance on its
            // own, which is a stall rather than a wait.
            if( !deadline.has_value() )
            {
                outcome = grab::fail( grab::ErrorCode::InternalFault,
                                      "the run stalled: no step can advance" );
                break;
            }

            const auto wake = *deadline > now ? *deadline : now + opaquePollPeriod;
            if( !timers.has_value() )
            {
                timers.emplace();
            }
            const int descriptor = timers->wake_fd();
            if( descriptor < 0 )
            {
                outcome = grab::fail( grab::ErrorCode::ProviderFailed,
                                      "the timer thread has no wake descriptor, so "
                                      "the run cannot be paced" );
                break;
            }
            const auto token = timers->arm( wake );

            // ceil, NOT duration_cast. poll() takes whole milliseconds, and
            // truncating means a 1.9 ms remainder becomes a 1 ms budget that
            // times out 0.9 ms EARLY -- the loop then cancels, re-arms and
            // waits again for the same deadline. With waypoint dwells of 4-6 ms
            // that misfired on roughly every other waypoint: the instrument
            // measured 1386 spurious wakes of 2767 drains, half of every wait
            // ending early, and 8301 syscall round trips to accomplish 2767
            // waits. Rounding up can only overshoot by under a millisecond, and
            // the deadline is absolute on the timer thread anyway, so the wake
            // is still governed by the timerfd rather than by this budget.
            wait_readable(
                descriptor,
                std::chrono::ceil<std::chrono::milliseconds>( wake - Clock::now() )
            );

            // DRAIN BEFORE CANCEL, and the order is load-bearing for the
            // measurement rather than for the run. cancel() drops the token
            // from `due_` as well as from the armed set -- that is its
            // contract -- so cancelling first destroyed every delivery before
            // it could be collected, and the timer thread's own spurious-wake
            // counter read 2403 of 2403 on a 199-step run: an artefact of this
            // loop, not a fact about the scheduler. Draining first collects
            // the token when the deadline really fired, leaving cancel to do
            // what it is here for -- retiring a wait that ended early.
            //
            // The token is discarded either way, so the run behaves
            // identically. What changes is that "spurious wake" now means one.
            ( void )timers->drain();
            timers->cancel( token );
        }

        // Whatever ended the loop, nothing may stay entered: an entered step
        // may hold a button, and interrupt() is the path that exits it.
        if( player.state() ==
            grab::sequence::PlayState::Playing ||
            player.state() == grab::sequence::PlayState::Paused )
        {
            ( void )player.interrupt();
        }

        // Harvested before `timers` leaves scope, which is the only moment it
        // can be: the TimerThread is a local of this function and no caller
        // ever holds one. stop() first, so the worker is joined and the
        // snapshot is final rather than a race with a last expiry -- and so
        // the nominal one-line summary lands here rather than during
        // destruction on some later line.
        if( timers.has_value() &&
            ( options.scheduling != nullptr || options.schedule != nullptr ) )
        {
            timers->stop();
            if( options.scheduling != nullptr )
            {
                *options.scheduling = timers->instrument();
            }
            if( options.schedule != nullptr )
            {
                *options.schedule = timers->counters();
            }
        }
        return outcome;
    }

}    // namespace grab::kernel::sequence
