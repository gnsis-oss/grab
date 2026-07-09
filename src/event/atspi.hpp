#pragma once

#include "grab/event.hpp"
#include "grab/result.hpp"

#include <cstdint>
#include <dbus/dbus.h>
#include <memory>
#include <optional>
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

    struct AtspiSignal
    {
            std::string interface;
            std::string member;
            std::string detail;
            std::string app;
            std::string role;
            std::string name;
    };

    [[nodiscard]]
    std::optional<grab::Event>
    decode_atspi_signal( const AtspiSignal& signal,
                         double             timestamp );

    class AtspiMonitor
    {
        public:

            [[nodiscard]]
            static grab::Result<AtspiMonitor>
            start( grab::core::Reactor& reactor,
                   grab::EventBus&      bus );

            ~AtspiMonitor();

            AtspiMonitor( const AtspiMonitor& ) = delete;
            AtspiMonitor&
            operator=( const AtspiMonitor& ) = delete;
            AtspiMonitor( AtspiMonitor&& other ) noexcept;
            AtspiMonitor&
            operator=( AtspiMonitor&& other ) noexcept;

            void
            stop() noexcept;

        private:

            struct State;

            static void
            handle_watch( const std::shared_ptr<State>& state,
                          DBusWatch*                    watch,
                          std::uint32_t                 events );

            [[nodiscard]]
            static dbus_bool_t
            add_watch( DBusWatch* watch,
                       void*      data );

            static void
            remove_watch( DBusWatch* watch,
                          void*      data ) noexcept;

            static void
            toggle_watch( DBusWatch* watch,
                          void*      data ) noexcept;

            [[nodiscard]]
            static std::uint64_t
            register_watch_locked( State&     state,
                                   DBusWatch* watch );

            [[nodiscard]]
            static bool
            has_registered_watch_locked( const State& state,
                                         DBusWatch*   watch ) noexcept;

            AtspiMonitor( grab::core::Reactor&   reactor,
                          std::shared_ptr<State> state ) noexcept;

            grab::core::Reactor*   reactor_ = nullptr;
            std::shared_ptr<State> state_;
    };

}    // namespace grab::event
