#pragma once

#include "grab/event.hpp"
#include "grab/result.hpp"

#include <cstddef>
#include <cstdint>
#include <dbus/dbus.h>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
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

    // The accessible object that emitted a signal, addressed on the a11y bus.
    struct AtspiObjectRef
    {
            std::string sender;    // D-Bus unique name, e.g. ":1.42"
            std::string path;      // object path of the emitting accessible
    };

    // Human identity resolved for an emitting object. An empty field means
    // the property was unavailable.
    struct AtspiIdentity
    {
            std::string app;    // friendly application name, e.g. "Firefox"
    };

    // Resolves an object reference to a human identity over the a11y bus.
    // Injected so the caching/enrichment logic is testable without a live
    // bus; returns nullopt when the object cannot be queried.
    using AtspiIdentityResolver =
        std::function<std::optional<AtspiIdentity>( const AtspiObjectRef& )>;

    // Replaces the signal's raw D-Bus sender with a resolved friendly
    // application name when one is available; other fields are preserved.
    // Raw AT-SPI signals carry the bus sender (":1.1") but no app name, so
    // without this a decoded event's app is an opaque connection id.
    void
    enrich_atspi_signal( AtspiSignal&                        signal,
                         const std::optional<AtspiIdentity>& identity );

    // Wraps a resolver with a per-sender application-name cache: every
    // accessible of one application shares its bus sender, so the friendly
    // name is queried at most once per application even under the visibility
    // signal bursts a busy desktop emits.
    class AtspiIdentityCache
    {
        public:

            explicit AtspiIdentityCache( AtspiIdentityResolver resolve ) noexcept;

            [[nodiscard]]
            std::optional<AtspiIdentity>
            resolve( const AtspiObjectRef& object );

        private:

            AtspiIdentityResolver                                            resolve_;
            std::map<std::string, std::optional<AtspiIdentity>, std::less<>> by_sender_;
    };

    // Refcounted, demand-driven AT-SPI event registration. acquire()/release() track a
    // per-event demand count; the injected registrar performs the actual D-Bus
    // RegisterEvent (enable==true) / DeregisterEvent (enable==false) only on the
    // 0->1 and 1->0 transitions. Thread-safe.
    class AtspiEventRegistry
    {
        public:

            using Registrar =
                std::function<grab::Result<void>( std::string_view atspi_event,
                                                  bool             enable )>;

            explicit AtspiEventRegistry( Registrar registrar ) noexcept;

            // 0->1 transition invokes registrar(name, true) and returns its Result;
            // subsequent acquires just bump the count and return {}.
            [[nodiscard]]
            grab::Result<void>
            acquire( std::string_view atspi_event );

            // 1->0 transition invokes registrar(name, false); best-effort (errors
            // swallowed).
            void
            release( std::string_view atspi_event ) noexcept;

            [[nodiscard]]
            std::size_t
            demand( std::string_view atspi_event ) const noexcept;

        private:

            Registrar                                       registrar_;
            mutable std::mutex                              mutex_;
            std::map<std::string, std::size_t, std::less<>> refcounts_;
    };

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

            // Acquire/release demand for the full built-in AT-SPI event set. Called by
            // the runtime when subscriber demand for accessibility kinds
            // appears/disappears.
            [[nodiscard]]
            grab::Result<void>
            enable_events();

            void
            disable_events() noexcept;

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
