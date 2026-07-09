#pragma once

#include "grab/result.hpp"

#include <cstdint>
#include <memory>

struct xcb_connection_t;

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

    class XInput2Monitor
    {
        public:

            [[nodiscard]]
            static grab::Result<XInput2Monitor>
            start( const char*          display,
                   grab::core::Reactor& reactor,
                   grab::EventBus&      bus );

            ~XInput2Monitor();

            XInput2Monitor( const XInput2Monitor& ) = delete;
            XInput2Monitor&
            operator=( const XInput2Monitor& ) = delete;
            XInput2Monitor( XInput2Monitor&& other ) noexcept;
            XInput2Monitor&
            operator=( XInput2Monitor&& other ) noexcept;

            void
            stop() noexcept;

        private:

            struct State;

            static void
            handle_fd( const std::shared_ptr<State>& state,
                       std::uint32_t                 events );

            XInput2Monitor( grab::core::Reactor&   reactor,
                            std::uint64_t          token,
                            std::shared_ptr<State> state ) noexcept;

            grab::core::Reactor*   reactor_ = nullptr;
            std::uint64_t          token_   = 0U;
            std::shared_ptr<State> state_;
    };

}    // namespace grab::event
