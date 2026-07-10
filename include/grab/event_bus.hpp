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

            static constexpr std::size_t defaultQueueDepth = 1'024U;

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
            subscribe( EventFilter filter,
                       std::size_t max_queue = defaultQueueDepth );

        private:

            std::shared_ptr<detail::EventBusState> state_;
    };

}    // namespace grab
