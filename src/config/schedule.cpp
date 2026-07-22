#include "config/schedule.hpp"
#include "grab/config.hpp"

#include <chrono>
#include <cstdint>
#include <limits>

namespace grab::config
{
    namespace
    {

        constexpr std::uint64_t maximumSkippedCaptures =
            std::numeric_limits<std::uint64_t>::max();

    }    // namespace

    WatchSchedule::WatchSchedule( std::chrono::milliseconds interval,
                                  const ScriptSection*      script ) :
        interval_( interval ),
        script_( script ),
        script_active_( script_ != nullptr && !script_->steps.empty() )
    {
    }

    Due
    WatchSchedule::next( std::chrono::steady_clock::time_point now )
    {
        initialize( now );
        account_for_overrun( now );

        if( !script_active_ || capture_at_ <= step_at_ )
        {
            return {
                .kind       = DueKind::Capture,
                .step_index = {},
                .wake_at    = capture_at_,
            };
        }
        return {
            .kind       = DueKind::Step,
            .step_index = step_index_,
            .wake_at    = step_at_,
        };
    }

    void
    WatchSchedule::capture_done( std::chrono::steady_clock::time_point now )
    {
        initialize( now );
        capture_at_ = now + interval_;
    }

    void
    WatchSchedule::step_done( std::chrono::steady_clock::time_point now )
    {
        initialize( now );
        if( !script_active_ )
        {
            return;
        }

        ++step_index_;
        if( step_index_ == script_->steps.size() )
        {
            if( !script_->loop )
            {
                script_active_ = false;
                return;
            }
            step_index_ = {};
        }
        arm_step( now );
    }

    void
    WatchSchedule::fail_script()
    {
        script_active_ = false;
    }

    std::uint64_t
    WatchSchedule::skipped_captures() const noexcept
    {
        return skipped_captures_;
    }

    void
    WatchSchedule::initialize( TimePoint now )
    {
        if( initialized_ )
        {
            return;
        }

        capture_at_ = now;
        if( script_active_ )
        {
            arm_step( now );
        }
        initialized_ = true;
    }

    void
    WatchSchedule::arm_step( TimePoint now )
    {
        step_at_               = now;
        const ScriptStep& step = script_->steps.at( step_index_ );
        if( step.action == StepAction::Delay )
        {
            step_at_ += std::chrono::milliseconds{ step.delay_ms };
        }
    }

    void
    WatchSchedule::account_for_overrun( TimePoint now )
    {
        if( interval_ <= std::chrono::milliseconds::zero() || now <= capture_at_ )
        {
            return;
        }

        const auto elapsed_intervals = ( now - capture_at_ ) / interval_;
        if( elapsed_intervals <= decltype( elapsed_intervals ){} )
        {
            return;
        }

        capture_at_        += interval_ * elapsed_intervals;
        const auto skipped  = static_cast<std::uint64_t>( elapsed_intervals );
        if( maximumSkippedCaptures - skipped_captures_ < skipped )
        {
            skipped_captures_ = maximumSkippedCaptures;
            return;
        }
        skipped_captures_ += skipped;
    }

}    // namespace grab::config
