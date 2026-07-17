#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/result.hpp"

#include <chrono>
#include <cstdint>
#include <memory>

namespace grab
{

    class EventBus;

}    // namespace grab

namespace grab::core
{

    class Reactor;

}    // namespace grab::core

namespace grab::drivers::desktop::x11
{

    class WindowTracker
    {
        public:

            [[nodiscard]]
            static grab::Result<WindowTracker>
            start( const char*               display,
                   grab::core::Reactor&      reactor,
                   grab::EventBus&           bus,
                   std::chrono::milliseconds poll_interval = std::chrono::milliseconds{
                       100
                   } );

            ~WindowTracker();

            WindowTracker( const WindowTracker& ) = delete;
            WindowTracker&
            operator=( const WindowTracker& ) = delete;
            WindowTracker( WindowTracker&& other ) noexcept;
            WindowTracker&
            operator=( WindowTracker&& other ) noexcept;

            void
            stop() noexcept;

        private:

            struct State;

            static void
            handle_fd( const std::shared_ptr<State>& state,
                       std::uint32_t                 events );

            static void
            handle_poll( const std::shared_ptr<State>& state );

            WindowTracker( grab::core::Reactor&   reactor,
                           std::uint64_t          fd_token,
                           std::uint64_t          timer_token,
                           std::shared_ptr<State> state ) noexcept;

            grab::core::Reactor*   reactor_     = nullptr;
            std::uint64_t          fd_token_    = 0U;
            std::uint64_t          timer_token_ = 0U;
            std::shared_ptr<State> state_;
    };

}    // namespace grab::drivers::desktop::x11
