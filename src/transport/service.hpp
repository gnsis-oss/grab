#pragma once

#include "eventgrab/v1/service.grpc.pb.h"
#include "grab/context.hpp"
#include "grab/event_bus.hpp"
#include "grab/process_ref.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace grab
{

    class ActiveKindProbe;
    class Session;

}    // namespace grab

namespace grab::transport
{

    struct AdmissionPolicy
    {
            static constexpr std::size_t defaultConcurrencyCap = 64U;
            static constexpr std::size_t defaultQueueCapacity  = 128U;
            static constexpr auto     defaultCallDeadline = std::chrono::seconds{ 30 };

            std::size_t               concurrency_cap{ defaultConcurrencyCap };
            std::size_t               queue_capacity{ defaultQueueCapacity };
            std::chrono::milliseconds per_call_deadline{ defaultCallDeadline };
            const std::atomic_bool*   healthy{ nullptr };
            std::string_view          unhealthy_reason{
                "admission rejected: daemon health check failed",
            };
    };

    struct ServiceOptions
    {
            static constexpr auto defaultPollInterval = std::chrono::milliseconds{ 100 };

            std::chrono::milliseconds poll_interval   = defaultPollInterval;
            AdmissionPolicy           admission{};
    };

    enum class PeerCloseReason : std::uint8_t
    {
        Deliberate,
        UnexpectedDisconnect,
    };

    struct SessionCredentials
    {
            std::string session;
            std::string token;
    };

    // Owns the resources associated with daemon peers. Its only process-bearing
    // API accepts OwnedProcess, so teardown cannot regress to PID/name scans.
    class PeerSessionRegistry
    {
        public:

            using Teardown =
                std::function<void( std::string_view, const std::vector<std::string>& )>;

            class CatalogScope
            {
                public:

                    CatalogScope( CatalogScope&& other ) noexcept;
                    CatalogScope&
                    operator=( CatalogScope&& other ) noexcept;
                    CatalogScope( const CatalogScope& ) = delete;
                    CatalogScope&
                    operator=( const CatalogScope& ) = delete;
                    ~CatalogScope();

                private:

                    friend class PeerSessionRegistry;
                    CatalogScope( PeerSessionRegistry&            registry,
                                  std::string                     peer,
                                  std::unordered_set<std::string> snapshot ) noexcept;

                    void
                                                    finish() noexcept;

                    PeerSessionRegistry*            registry_{ nullptr };
                    std::string                     peer_;
                    std::unordered_set<std::string> snapshot_;
            };

            explicit PeerSessionRegistry( Teardown teardown = {} );
            ~PeerSessionRegistry();

            PeerSessionRegistry( const PeerSessionRegistry& ) = delete;
            PeerSessionRegistry&
            operator=( const PeerSessionRegistry& )      = delete;
            PeerSessionRegistry( PeerSessionRegistry&& ) = delete;
            PeerSessionRegistry&
            operator=( PeerSessionRegistry&& ) = delete;

            [[nodiscard]]
            SessionCredentials
            open( std::string peer );

            [[nodiscard]]
            grpc::Status
            adopt( std::string      peer,
                   std::string_view session,
                   std::string_view token );

            [[nodiscard]]
            grpc::Status
            add_resource( std::string_view peer,
                          std::string      resource );

            [[nodiscard]]
            grpc::Status
            add_process( std::string_view   peer,
                         std::string        resource,
                         grab::OwnedProcess process );

            [[nodiscard]]
            CatalogScope
            scope( std::string_view peer );

            void
            close( std::string_view peer,
                   PeerCloseReason  reason ) noexcept;

            void
            deliberate_close( std::string_view peer ) noexcept;

            void
            unexpected_disconnect( std::string_view peer ) noexcept;

            [[nodiscard]]
            bool
            active( std::string_view peer ) const;

            [[nodiscard]]
            std::size_t
            close_count() const;

        private:

            struct Resource
            {
                    std::optional<grab::OwnedProcess> process;
            };

            struct Session
            {
                    std::string                               token;
                    std::string                               peer;
                    std::unordered_map<std::string, Resource> resources;
            };

            [[nodiscard]]
            std::unordered_set<std::string>
            catalog_snapshot( std::string_view peer ) const;

            void
            reap_diff( std::string_view                       peer,
                       const std::unordered_set<std::string>& snapshot ) noexcept;

            static void
            terminate_owned( std::vector<Resource>& resources ) noexcept;

            mutable std::mutex                           mutex_;
            std::unordered_map<std::string, Session>     sessions_;
            std::unordered_map<std::string, std::string> active_peers_;
            Teardown                                     teardown_;
            std::uint64_t                                next_session_{ 1U };
            std::size_t                                  close_count_{};
    };

    class EventService final : public eventgrab::v1::EventGrabService::Service
    {
        public:

            explicit EventService( grab::EventBus&              bus,
                                   const grab::ActiveKindProbe* probe   = nullptr,
                                   ServiceOptions               options = {},
                                   grab::Session* session = nullptr ) noexcept;
            ~EventService() override;

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

            grpc::Status
            SetClientContext( grpc::ServerContext*,
                              const eventgrab::v1::SetClientContextRequest*,
                              eventgrab::v1::SetClientContextResponse* ) override;

            grpc::Status
            ResolveNode( grpc::ServerContext*,
                         const eventgrab::v1::ResolveNodeRequest*,
                         eventgrab::v1::ResolveNodeResponse* ) override;

            grpc::Status
            PerformAction( grpc::ServerContext*,
                           const eventgrab::v1::PerformActionRequest*,
                           eventgrab::v1::PerformActionResponse* ) override;

            grpc::Status
            CaptureFrame( grpc::ServerContext*,
                          const eventgrab::v1::CaptureFrameRequest*,
                          eventgrab::v1::CaptureFrameResponse* ) override;

            [[nodiscard]]
            std::size_t
            registered_rpc_count() const noexcept;

            [[nodiscard]]
            std::size_t
            wrapped_rpc_count( std::string_view rpc_name ) const noexcept;

        private:

            class AdmissionController;

            [[nodiscard]]
            grpc::Status
                                                           dispatch(
                                                               std::string_view                                              rpc_name,
                                                               grpc::ServerContext*                                          context,
                                                               const std::function<grpc::Status( grab::OperationContext& )>& work
                                                           );

            grab::EventBus*                                bus_     = nullptr;
            const grab::ActiveKindProbe*                   probe_   = nullptr;
            grab::Session*                                 session_ = nullptr;
            ServiceOptions                                 options_;
            std::unique_ptr<AdmissionController>           admission_;
            PeerSessionRegistry                            sessions_;
            mutable std::mutex                             client_context_mutex_;
            std::unordered_map<std::string, std::uint64_t> client_context_sequence_;
    };

}    // namespace grab::transport
