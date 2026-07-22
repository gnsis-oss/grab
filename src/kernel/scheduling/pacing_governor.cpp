#include "grab/result.hpp"
#include "kernel/scheduling/pacing_governor.hpp"

#include <chrono>
#include <cstdint>

namespace grab::kernel::scheduling
{
    namespace
    {

        constexpr std::int64_t  nanosecondsPerSecond = 1'000'000'000;
        constexpr std::uint32_t minimumFrameRate     = 1U;

    }    // namespace

    grab::Result<PacingGovernor>
    PacingGovernor::for_fps( std::uint32_t frames_per_second )
    {
        if( frames_per_second < minimumFrameRate )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "recording frame rate must be greater than zero" );
        }

        return PacingGovernor{ std::chrono::nanoseconds{
            nanosecondsPerSecond / static_cast<std::int64_t>( frames_per_second )
        } };
    }

    PacingGovernor::PacingGovernor( std::chrono::nanoseconds interval ) noexcept :
        interval_{ interval }
    {
    }

    std::chrono::nanoseconds
    PacingGovernor::interval() const noexcept
    {
        return interval_;
    }

    std::chrono::steady_clock::time_point
    PacingGovernor::next_deadline(
        std::chrono::steady_clock::time_point from
    ) const noexcept
    {
        return from + interval_;
    }

}    // namespace grab::kernel::scheduling
