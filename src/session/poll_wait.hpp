#pragma once

#include <chrono>
#include <cstdint>
#include <functional>

namespace grab::session
{

    // What one round of a bounded wait learned.
    enum class Probe : std::uint8_t
    {
        Ready,        // the thing being waited for is up
        Retry,        // not yet — ask again after the interval
        Abandoned,    // it will never be up (the service died); stop waiting
    };

    inline constexpr auto defaultPollInterval = std::chrono::milliseconds{ 25 };

    // Runs `probe` immediately, then every `interval`, until it answers Ready
    // or Abandoned or `timeout` elapses; a timeout is reported as Retry.
    //
    // Every service brought up on a display becomes ready asynchronously and
    // announces it in a different way — a socket appears, a selection changes
    // owner, a root-window property is set — so the *waiting* is the only part
    // they share. Keeping it here is also what keeps grab's one sleeping loop
    // to one place (tests/scripts/sleep_allowlist.txt).
    [[nodiscard]]
    Probe
    poll_until( const std::function<Probe()>& probe,
                std::chrono::milliseconds     timeout,
                std::chrono::milliseconds     interval = defaultPollInterval );

}    // namespace grab::session
