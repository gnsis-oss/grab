#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

// This header owns the public subscription surface.
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

}    // namespace grab
