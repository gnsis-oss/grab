// PLACEHOLDER — registered in tests/CMakeLists.txt by Phase 0 so the
// timer-thread unit can replace this file without touching a shared build file.
//
// The suite is named TimerThreadTiming because tests/CMakeLists.txt labels that
// suite "timing", so a loaded machine can exclude it with `ctest -LE timing`.

#include "kernel/scheduling/timer_thread.hpp"

// clang-format off
#include <gtest/gtest.h>
// clang-format on

namespace
{

    TEST( TimerThreadTiming,
          Placeholder )
    {
        const grab::kernel::scheduling::TimerThread timers;
        // The Phase 0 stub owns no fd; the timer-thread unit replaces it.
        EXPECT_LT( timers.wake_fd(), 0 );
    }

}    // namespace
