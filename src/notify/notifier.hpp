#ifndef GRAB_NOTIFY_NOTIFIER_HPP
#define GRAB_NOTIFY_NOTIFIER_HPP

#include "grab/result.hpp"

#include <cstdint>
#include <dbus/dbus.h>
#include <memory>
#include <string>

namespace grab::notify
{

    struct Notification
    {
            std::string  app_name;
            std::string  summary;
            std::string  body;
            std::string  icon;
            std::int32_t timeout_ms = -1;
    };

    struct DbusMessageDeleter
    {
            void
            operator()( DBusMessage* message ) const noexcept;
    };

    using DbusMessagePtr = std::unique_ptr<DBusMessage, DbusMessageDeleter>;

    [[nodiscard]]
    grab::Result<DbusMessagePtr>
    build_notify_message( const Notification& notification );

    class Notifier
    {
        public:

            [[nodiscard]]
            static grab::Result<Notifier>
            open();

            ~Notifier();

            Notifier( const Notifier& ) = delete;
            Notifier&
            operator=( const Notifier& ) = delete;
            Notifier( Notifier&& other ) noexcept;
            Notifier&
            operator=( Notifier&& other ) noexcept;

            [[nodiscard]]
            grab::Result<std::uint32_t>
            notify( const Notification& notification );

            [[nodiscard]]
            grab::Result<void>
            close( std::uint32_t id );

        private:

            struct State;

            explicit Notifier( std::unique_ptr<State> state ) noexcept;

            std::unique_ptr<State> state_;
    };

}    // namespace grab::notify

#endif    // GRAB_NOTIFY_NOTIFIER_HPP
