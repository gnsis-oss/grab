#pragma once

#include "grab/result.hpp"

#include <memory>
#include <string>

namespace grab
{

    class EventBus;

}    // namespace grab

namespace grpc
{

    class Server;

}    // namespace grpc

namespace grab::transport
{

    class EventService;

    class TransportServer
    {
        public:

            [[nodiscard]]
            static grab::Result<TransportServer>
            start( const std::string& endpoint,
                   grab::EventBus&    bus );

            ~TransportServer();

            TransportServer( const TransportServer& ) = delete;
            TransportServer&
            operator=( const TransportServer& ) = delete;
            TransportServer( TransportServer&& other ) noexcept;
            TransportServer&
            operator=( TransportServer&& other ) noexcept;

            void
            shutdown() noexcept;

            [[nodiscard]]
            const std::string&
            endpoint() const noexcept;

        private:

            TransportServer( std::string                   endpoint,
                             std::unique_ptr<EventService> service,
                             std::unique_ptr<grpc::Server> server ) noexcept;

            std::string                   endpoint_;
            std::unique_ptr<EventService> service_;
            std::unique_ptr<grpc::Server> server_;
    };

}    // namespace grab::transport
