#pragma once

#include "grab/event.hpp"
#include "grab/ids.hpp"
#include "grab/result.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace grab::detail
{

    class EventBusState;
    class SubscriptionState;

}    // namespace grab::detail

namespace grab
{

    enum class QueueOverflowPolicy : std::uint8_t
    {
        Coalesce,
        NeverDrop,
    };

    struct QueueOptions
    {
            static constexpr std::size_t         defaultCapacity = 1'024U;
            static constexpr QueueOverflowPolicy defaultOverflow =
                QueueOverflowPolicy::Coalesce;

            std::size_t         capacity = defaultCapacity;
            QueueOverflowPolicy overflow = defaultOverflow;
    };

    struct SubscriptionScope
    {
            std::vector<EventKind> kinds;
            EventFilter            filter;
    };

    struct QueueGapMarker
    {
            ErrorCode     code                    = ErrorCode::QueueGap;
            std::uint64_t last_delivered_sequence = 0U;
    };

    using SubscriptionEvent = std::variant<Event, QueueGapMarker>;

    class EventBus;

    class Subscription
    {
        public:

            Subscription() noexcept;
            ~Subscription();

            Subscription( const Subscription& ) = delete;
            Subscription&
            operator=( const Subscription& ) = delete;
            Subscription( Subscription&& other ) noexcept;
            Subscription&
            operator=( Subscription&& other ) noexcept;

            [[nodiscard]]
            std::optional<Event>
            try_pop();

            [[nodiscard]]
            std::optional<SubscriptionEvent>
            try_pop_item();

            [[nodiscard]]
            SubscriptionId
            id() const noexcept;

            [[nodiscard]]
            SubscriptionScope
            scope() const;

            [[nodiscard]]
            std::uint64_t
            overflow_count() const noexcept;

            [[nodiscard]]
            bool
            lagging() const noexcept;

            [[nodiscard]]
            bool
            needs_resync() const noexcept;

            [[nodiscard]]
            std::uint64_t
            dropped_count() const noexcept;

            void
            set_notify( std::function<void()> on_data );

        private:

            friend class EventBus;

            Subscription( std::weak_ptr<detail::EventBusState>       bus,
                          std::shared_ptr<detail::SubscriptionState> state ) noexcept;

            void
                                                       unsubscribe() noexcept;

            std::weak_ptr<detail::EventBusState>       bus_;
            std::shared_ptr<detail::SubscriptionState> state_;
    };

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
