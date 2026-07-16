#pragma once

#include "client/transport.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace grab::client
{

    struct DaemonOptions
    {
            std::string               endpoint;
            std::filesystem::path     socket_path;
            std::string               executable{ "grab" };
            std::chrono::milliseconds initial_backoff{ 10 };
            std::chrono::milliseconds maximum_backoff{ 100 };
            std::chrono::milliseconds startup_timeout{ 2'000 };
            std::chrono::milliseconds termination_grace{ 250 };
    };

    [[nodiscard]]
    bool
    is_connection_error( const grab::Error& error ) noexcept;

    class Client
    {
        public:

            explicit Client( Transport& transport ) noexcept;
            explicit Client( std::unique_ptr<Transport> transport ) noexcept;
            Client( Transport&    transport,
                    DaemonOptions options );
            Client( std::unique_ptr<Transport> transport,
                    DaemonOptions              options );
            ~Client();

            Client( const Client& ) = delete;
            Client&
            operator=( const Client& ) = delete;
            Client( Client&& ) noexcept;
            Client&
            operator=( Client&& ) noexcept;

            [[nodiscard]]
            grab::Result<void>
            ensure_daemon();

            [[nodiscard]]
            grab::Result<void>
            ensure_daemon( DaemonOptions options );

            [[nodiscard]]
            grab::Result<grab::Match>
            resolve( const grab::Locator& locator,
                     grab::Cardinality    cardinality = grab::Cardinality::ExactlyOne );

            [[nodiscard]]
            grab::Result<grab::Receipt>
            perform( const grab::Action&        action,
                     const grab::ActionOptions& options = {} );

            [[nodiscard]]
            grab::Result<grab::Frame>
            capture( const grab::CaptureTarget&  target,
                     const grab::CaptureOptions& options = {} );

            [[nodiscard]]
            grab::Result<void>
            push_event( grab::Event event );

            [[nodiscard]]
            grab::Result<SubscriptionHandle>
            subscribe( grab::EventFilter filter );

            [[nodiscard]]
            grab::Result<std::vector<grab::EventTypeDescriptor>>
            list_event_types();

        private:

            class State;
            std::shared_ptr<State> state_;
    };

}    // namespace grab::client
