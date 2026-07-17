#pragma once

#include "grab/event.hpp"
#include "grab/ids.hpp"
#include "grab/result.hpp"
#include "grab/watch.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

namespace grab
{

    class EventBus
    {
        public:

            using SnapshotProvider = std::function<std::vector<Event>()>;
            using DemandCallback   = std::function<void( EventKind, bool )>;

            static constexpr std::size_t defaultQueueDepth =
                QueueOptions::defaultCapacity;

            EventBus();
            ~EventBus();

            EventBus( const EventBus& ) = delete;
            EventBus&
            operator=( const EventBus& ) = delete;
            EventBus( EventBus&& )       = delete;
            EventBus&
            operator=( EventBus&& ) = delete;

            void
            publish( Event event ) noexcept;

            [[nodiscard]]
            Subscription
            subscribe( EventFilter  filter,
                       QueueOptions options = {} );

            [[nodiscard]]
            Subscription
            subscribe( SubscriptionScope scope,
                       QueueOptions      options = {} );

            [[nodiscard]]
            Subscription
            subscribe( EventFilter filter,
                       std::size_t max_queue );

            void
            register_snapshot_provider( EventKind        kind,
                                        SnapshotProvider provider );

            void
            unregister_snapshot_provider( EventKind kind );

            void
            set_demand_callback( DemandCallback callback );

            [[nodiscard]]
            std::size_t
            subscription_refcount( EventKind kind ) const noexcept;

        private:

            std::shared_ptr<detail::EventBusState> state_;
    };

}    // namespace grab
