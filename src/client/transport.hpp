#pragma once

#include "grab/capture.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/event_descriptor.hpp"
#include "grab/interaction.hpp"
#include "grab/locator.hpp"
#include "grab/query.hpp"
#include "grab/result.hpp"
#include "grab/trace.hpp"

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
            virtual grab::Result<grab::Match>
            resolve( const grab::Locator& locator,
                     grab::Cardinality    cardinality ) = 0;

            [[nodiscard]]
            virtual grab::Result<grab::Receipt>
            perform( const grab::Action&        action,
                     const grab::ActionOptions& options ) = 0;

            [[nodiscard]]
            virtual grab::Result<grab::Frame>
            capture( const grab::CaptureTarget&  target,
                     const grab::CaptureOptions& options ) = 0;

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
