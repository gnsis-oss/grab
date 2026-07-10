#pragma once

#include "grab/result.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace grab
{

    class EventBus;

}    // namespace grab

namespace grab::event
{

    class EventSource;

}    // namespace grab::event

namespace grab::service
{

    struct DaemonOptions
    {
            std::string                          endpoint = "unix:/tmp/grab.sock";
            std::optional<std::filesystem::path> store_dir;
            std::function<std::vector<std::unique_ptr<grab::event::EventSource>>()>
                source_factory;
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
