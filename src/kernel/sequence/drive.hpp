#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

// Pump a Player to completion against the real clock. Caller-driven
// everywhere else in this design; here the caller is the process, so this is
// where a real clock is finally read. It does not sleep -- the wait is a
// TimerThread deadline and a poll() on its eventfd, which is the sanctioned
// mechanism and the reason src/ carries no raw sleeps.
//
// Moved out of the CLI so the public sequence API (grab/sequence.hpp) and
// `grab play` share exactly one pump loop. The CLI's signal handling and the
// API's cancellation token both arrive through `cancelled` below, so the
// loop itself owns neither policy.

#include "grab/result.hpp"
#include "kernel/scheduling/timer_thread.hpp"
#include "kernel/sequence/player.hpp"
#include "kernel/support/step_diag.hpp"

#include <functional>
#include <string_view>

namespace grab::kernel::sequence
{

    struct DriveOptions
    {
            // Runs once per pump, on this loop's own thread, including on the
            // last iteration so the tail of a run is not dropped. The CLI's
            // --trail drains its observation queue here.
            // NOLINTNEXTLINE(readability-redundant-member-init)
            std::function<void()>   on_pump{};

            // Polled once per pump. Answering true turns the run into an
            // ordinary unwind -- interrupt() exits every entered step in
            // reverse and reaps the holds the completed ones left down --
            // and drive() reports `cancel_reason` as the outcome.
            // NOLINTNEXTLINE(readability-redundant-member-init)
            std::function<bool()>   cancelled{};

            // The message the Cancelled outcome carries, so a signal and an
            // embedder's kill-switch each read as what they were.
            std::string_view        cancel_reason{ "the run was cancelled" };

            // Where the timer thread's accuracy lands, when the caller wants
            // it. Harvested HERE and nowhere else because the TimerThread is
            // a local of drive(): it is created on the first wait and
            // destroyed on return, so a caller has no other moment at which
            // to ask it anything.
            grab::diag::Instrument* scheduling{ nullptr };
            grab::kernel::scheduling::ScheduleCounters* schedule{ nullptr };
    };

    // Fails only when a step failed under ErrorPolicy::Abort or `cancelled`
    // answered true. The run is always left in a terminal state: a loop that
    // ends any other way interrupts the player first, so nothing stays
    // entered.
    [[nodiscard]]
    grab::Result<void>
    drive( Player&             player,
           const DriveOptions& options = {} );

}    // namespace grab::kernel::sequence
