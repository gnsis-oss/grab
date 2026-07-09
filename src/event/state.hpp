#pragma once

#include "grab/event.hpp"
#include "grab/pid.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace grab
{

    class EventBus;

}    // namespace grab

namespace grab::event::detail
{

    struct WindowRecord
    {
            std::string app;
            grab::Pid   pid;
            std::string title;
    };

}    // namespace grab::event::detail

namespace grab::event
{

    class StateManager
    {
        public:

            explicit StateManager();
            ~StateManager()                     = default;

            StateManager( const StateManager& ) = delete;
            StateManager&
            operator=( const StateManager& )        = delete;
            StateManager( StateManager&& ) noexcept = default;
            StateManager&
            operator=( StateManager&& ) noexcept = default;

            void
            observe( const grab::Event& event );

            [[nodiscard]]
            grab::Event
            snapshot( double timestamp ) const;

            void
            publish_snapshot( grab::EventBus& bus,
                              double          timestamp ) const;

            [[nodiscard]]
            std::size_t
            open_window_count() const noexcept;

        private:

            std::vector<detail::WindowRecord> open_windows_;
            detail::WindowRecord              focused_;
            bool                              has_focused_window_ = false;
    };

}    // namespace grab::event
