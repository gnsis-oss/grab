#pragma once

#include "grab/event.hpp"
#include "grab/result.hpp"

#include <cstdint>
#include <span>
#include <string_view>

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

    enum class SourceState : std::uint8_t
    {
        Idle,
        Running,
        Failed,
        Stopped,
    };

    class EventSource
    {
        public:

            EventSource()                     = default;
            virtual ~EventSource()            = default;

            EventSource( const EventSource& ) = delete;
            EventSource&
            operator=( const EventSource& ) = delete;
            EventSource( EventSource&& )    = delete;
            EventSource&
            operator=( EventSource&& ) = delete;

            [[nodiscard]]
            virtual grab::Result<void>
            start( grab::core::Reactor& reactor,
                   grab::EventBus&      bus ) = 0;

            virtual void
            stop() noexcept = 0;

            [[nodiscard]]
            virtual SourceState
            state() const noexcept = 0;

            [[nodiscard]]
            virtual std::string_view
            name() const noexcept = 0;

            [[nodiscard]]
            virtual std::span<const grab::EventKind>
            kinds() const noexcept = 0;
    };

}    // namespace grab::event
