#pragma once

#include "grab/result.hpp"

#include <cstddef>
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

    [[nodiscard]]
    std::string
    default_daemon_endpoint();

    struct DaemonOptions
    {
            static constexpr std::size_t         defaultStorageQueueDepth  = 65'536U;
            static constexpr std::size_t         defaultStorageBufferLimit = 1U;

            std::string                          endpoint = default_daemon_endpoint();
            std::optional<std::filesystem::path> store_dir;
            std::size_t storage_queue_depth  = defaultStorageQueueDepth;
            std::size_t storage_buffer_limit = defaultStorageBufferLimit;
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
