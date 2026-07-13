#pragma once

#include "eventgrab/v1/service.grpc.pb.h"
#include "grab/event_bus.hpp"

#include <chrono>

namespace grab
{

    class ActiveKindProbe;

}    // namespace grab

namespace grab::transport
{

    struct ServiceOptions
    {
            static constexpr auto defaultPollInterval = std::chrono::milliseconds{ 100 };

            std::chrono::milliseconds poll_interval   = defaultPollInterval;
    };

    class EventService final : public eventgrab::v1::EventGrabService::Service
    {
        public:

            explicit EventService( grab::EventBus&              bus,
                                   const grab::ActiveKindProbe* probe   = nullptr,
                                   ServiceOptions               options = {} ) noexcept;
            ~EventService() override            = default;

            EventService( const EventService& ) = delete;
            EventService&
            operator=( const EventService& ) = delete;
            EventService( EventService&& )   = delete;
            EventService&
            operator=( EventService&& ) = delete;

            grpc::Status
            PushEvent( grpc::ServerContext*,
                       const eventgrab::v1::PushEventRequest*,
                       eventgrab::v1::PushEventResponse* ) override;

            grpc::Status
            ListEventTypes( grpc::ServerContext*,
                            const eventgrab::v1::ListEventTypesRequest*,
                            eventgrab::v1::ListEventTypesResponse* ) override;

            grpc::Status
            Subscribe( grpc::ServerContext*,
                       const eventgrab::v1::EventFilter*,
                       grpc::ServerWriter<eventgrab::v1::Event>* ) override;

        private:

            grab::EventBus*              bus_   = nullptr;
            const grab::ActiveKindProbe* probe_ = nullptr;
            ServiceOptions               options_;
    };

}    // namespace grab::transport
