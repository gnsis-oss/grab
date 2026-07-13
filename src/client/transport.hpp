#pragma once

#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/event_descriptor.hpp"
#include "grab/result.hpp"

#include <memory>
#include <optional>
#include <vector>

namespace grab::client
{

    class SubscriptionStream
    {
        public:

            virtual ~SubscriptionStream() = default;

            [[nodiscard]]
            virtual grab::Result<std::optional<grab::SubscriptionEvent>>
            try_next() = 0;
    };

    using SubscriptionHandle = std::unique_ptr<SubscriptionStream>;

    class Transport
    {
        public:

            virtual ~Transport() = default;

            [[nodiscard]]
            virtual grab::Result<void>
            push_event( grab::Event event ) = 0;

            [[nodiscard]]
            virtual grab::Result<SubscriptionHandle>
            subscribe( grab::EventFilter filter ) = 0;

            [[nodiscard]]
            virtual grab::Result<std::vector<grab::EventTypeDescriptor>>
            list_event_types() = 0;
    };

}    // namespace grab::client
