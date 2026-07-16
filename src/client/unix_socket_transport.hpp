#pragma once

#include "client/transport.hpp"

#include <memory>
#include <string>

namespace grab::client
{

    class UnixSocketTransport final : public Transport
    {
        public:

            explicit UnixSocketTransport( std::string endpoint );
            ~UnixSocketTransport() override;

            UnixSocketTransport( const UnixSocketTransport& ) = delete;
            UnixSocketTransport&
            operator=( const UnixSocketTransport& ) = delete;
            UnixSocketTransport( UnixSocketTransport&& ) noexcept;
            UnixSocketTransport&
            operator=( UnixSocketTransport&& ) noexcept;

            [[nodiscard]]
            grab::Result<grab::Match>
            resolve( const grab::Locator& locator,
                     grab::Cardinality    cardinality ) override;

            [[nodiscard]]
            grab::Result<grab::Receipt>
            perform( const grab::Action&        action,
                     const grab::ActionOptions& options ) override;

            [[nodiscard]]
            grab::Result<grab::Frame>
            capture( const grab::CaptureTarget&  target,
                     const grab::CaptureOptions& options ) override;

            [[nodiscard]]
            grab::Result<void>
            push_event( grab::Event event ) override;

            [[nodiscard]]
            grab::Result<SubscriptionHandle>
            subscribe( grab::EventFilter filter ) override;

            [[nodiscard]]
            grab::Result<std::vector<grab::EventTypeDescriptor>>
            list_event_types() override;

            [[nodiscard]]
            const std::string&
            endpoint() const noexcept;

        private:

            class Impl;

            std::string           endpoint_;
            std::unique_ptr<Impl> impl_;
    };

}    // namespace grab::client
