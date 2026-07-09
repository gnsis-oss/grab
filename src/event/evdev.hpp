#ifndef GRAB_EVENT_EVDEV_HPP
#define GRAB_EVENT_EVDEV_HPP

#include "grab/result.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace grab
{

    class EventBus;

}    // namespace grab

namespace grab::core
{

    class Reactor;

}    // namespace grab::core

namespace grab::event
{

    class EvdevMonitor
    {
        public:

            [[nodiscard]]
            static grab::Result<EvdevMonitor>
            open_device( const std::string&   path,
                         grab::core::Reactor& reactor,
                         grab::EventBus&      bus );

            [[nodiscard]]
            static grab::Result<EvdevMonitor>
            adopt_fd( int                  fd,
                      grab::core::Reactor& reactor,
                      grab::EventBus&      bus );

            ~EvdevMonitor();

            EvdevMonitor( const EvdevMonitor& ) = delete;
            EvdevMonitor&
            operator=( const EvdevMonitor& ) = delete;
            EvdevMonitor( EvdevMonitor&& other ) noexcept;
            EvdevMonitor&
            operator=( EvdevMonitor&& other ) noexcept;

            void
            stop() noexcept;

        private:

            struct State;

            static void
            handle_fd( const std::shared_ptr<State>& state,
                       std::uint32_t                 events );

            EvdevMonitor( grab::core::Reactor&   reactor,
                          std::uint64_t          token,
                          std::shared_ptr<State> state ) noexcept;

            grab::core::Reactor*   reactor_ = nullptr;
            std::uint64_t          token_   = 0U;
            std::shared_ptr<State> state_;
    };

}    // namespace grab::event

#endif    // GRAB_EVENT_EVDEV_HPP
