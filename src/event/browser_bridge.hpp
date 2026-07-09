#pragma once

#include "grab/event.hpp"
#include "grab/result.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

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

    [[nodiscard]]
    grab::Result<grab::Event>
    parse_browser_message( std::string_view json );

    class BrowserBridge
    {
        public:

            [[nodiscard]]
            static grab::Result<BrowserBridge>
            start( int                  input_fd,
                   grab::core::Reactor& reactor,
                   grab::EventBus&      bus );

            ~BrowserBridge();

            BrowserBridge( const BrowserBridge& ) = delete;
            BrowserBridge&
            operator=( const BrowserBridge& ) = delete;
            BrowserBridge( BrowserBridge&& other ) noexcept;
            BrowserBridge&
            operator=( BrowserBridge&& other ) noexcept;

            void
            stop() noexcept;

        private:

            struct State;

            static void
            handle_fd( const std::shared_ptr<State>& state,
                       std::uint32_t                 events );

            [[nodiscard]]
            static int
            active_input_fd( const std::shared_ptr<State>& state );

            static void
            append_read_bytes( State&                state,
                               std::span<const char> bytes );

            static void
            drain_complete_frames( State&                    state,
                                   std::vector<grab::Event>& pending_events );

            static void
            apply_terminal_events( State&        state,
                                   std::uint32_t events ) noexcept;

            BrowserBridge( grab::core::Reactor&   reactor,
                           std::uint64_t          token,
                           std::shared_ptr<State> state ) noexcept;

            grab::core::Reactor*   reactor_ = nullptr;
            std::uint64_t          token_   = 0U;
            std::shared_ptr<State> state_;
    };

}    // namespace grab::event
