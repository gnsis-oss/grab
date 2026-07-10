#pragma once

#include "event/source.hpp"
#include "grab/event.hpp"
#include "grab/result.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <span>
#include <string_view>

namespace grab::event::detail
{

    [[nodiscard]]
    grab::EventFilter
    state_source_filter();

}    // namespace grab::event::detail

namespace grab::event
{

    inline constexpr auto defaultStateSourceInterval = std::chrono::seconds{ 60 };

    class StateSource final : public EventSource
    {
        public:

            explicit StateSource( std::chrono::nanoseconds interval =
                                      defaultStateSourceInterval );
            ~StateSource() override;

            StateSource( const StateSource& ) = delete;
            StateSource&
            operator=( const StateSource& ) = delete;
            StateSource( StateSource&& )    = delete;
            StateSource&
            operator=( StateSource&& ) = delete;

            [[nodiscard]]
            grab::Result<void>
            start( grab::core::Reactor& reactor,
                   grab::EventBus&      bus ) override;

            void
            stop() noexcept override;

            [[nodiscard]]
            SourceState
            state() const noexcept override;

            [[nodiscard]]
            std::string_view
            name() const noexcept override;

            [[nodiscard]]
            std::span<const grab::EventKind>
            kinds() const noexcept override;

        private:

            struct State;

            static void
            drain( const std::shared_ptr<State>& state );

            static void
            publish_snapshot_once( const std::shared_ptr<State>& state );

            static void
            publish_periodic_snapshot( const std::shared_ptr<State>& state );

            static void
            post_drain( const std::weak_ptr<State>& weak_state );

            static void
            deactivate_state( const std::shared_ptr<State>& state ) noexcept;

            std::chrono::nanoseconds interval_;
            std::shared_ptr<State>   state_;
            std::atomic<SourceState> source_state_{ SourceState::Idle };
    };

}    // namespace grab::event
