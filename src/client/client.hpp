#pragma once

#include "client/transport.hpp"

#include <memory>
#include <vector>

namespace grab::client
{

    class Client
    {
        public:

            explicit Client( Transport& transport ) noexcept;
            explicit Client( std::unique_ptr<Transport> transport ) noexcept;

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

            [[nodiscard]]
            grab::Result<Transport*>
                                       transport() noexcept;

            std::unique_ptr<Transport> owned_transport_;
            Transport*                 transport_ = nullptr;
    };

}    // namespace grab::client
