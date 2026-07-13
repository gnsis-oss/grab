#pragma once

#include "grab/event.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

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
            std::uint64_t
            overflow_count() const noexcept;

            [[nodiscard]]
            bool
            lagging() const noexcept;

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
            subscribe( EventFilter filter,
                       std::size_t max_queue );

        private:

            std::shared_ptr<detail::EventBusState> state_;
    };

}    // namespace grab
