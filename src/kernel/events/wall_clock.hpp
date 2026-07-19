#pragma once

#include <chrono>

namespace grab::kernel
{

    // Uniform event-timestamp clock: wall-clock epoch seconds as double.
    // Every producer that stamps grab::Event::timestamp must use this.
    [[nodiscard]]
    inline double
    now_timestamp_s()
    {
        const auto duration = std::chrono::system_clock::now().time_since_epoch();
        return std::chrono::duration<double>( duration ).count();
    }

}    // namespace grab::kernel
