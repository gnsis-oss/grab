#ifndef GRAB_SERVICE_DAEMON_HPP
#define GRAB_SERVICE_DAEMON_HPP

#include "grab/result.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace grab
{

    class EventBus;

}    // namespace grab

namespace grab::service
{

    struct DaemonOptions
    {
            std::string                          endpoint = "unix:/tmp/grab.sock";
            std::optional<std::filesystem::path> store_dir;
    };

    class Daemon
    {
        public:

            [[nodiscard]]
            static grab::Result<Daemon>
            start( DaemonOptions options );

            ~Daemon();

            Daemon( const Daemon& ) = delete;
            Daemon&
            operator=( const Daemon& ) = delete;
            Daemon( Daemon&& other ) noexcept;
            Daemon&
            operator=( Daemon&& other ) noexcept;

            void
            shutdown() noexcept;

            [[nodiscard]]
            grab::EventBus&
            bus() noexcept;

            [[nodiscard]]
            const std::string&
            endpoint() const noexcept;

        private:

            class Impl;

            explicit Daemon( std::unique_ptr<Impl> impl ) noexcept;

            std::unique_ptr<Impl> impl_;
    };

}    // namespace grab::service

#endif    // GRAB_SERVICE_DAEMON_HPP
