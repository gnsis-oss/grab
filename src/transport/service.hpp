#pragma once

#include "eventgrab/v1/service.grpc.pb.h"
#include "grab/event_bus.hpp"

namespace grab
{

    class ActiveKindProbe;

}    // namespace grab

namespace grab::transport
{

    class EventService final : public eventgrab::v1::EventGrabService::Service
    {
        public:

            explicit EventService( grab::EventBus&              bus,
                                   const grab::ActiveKindProbe* probe =
                                       nullptr ) noexcept;
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
    };

}    // namespace grab::transport
