#include "grab/event.hpp"
#include "grab/event_bus.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace grab
{
    namespace
    {

        constexpr std::uint64_t firstSequence = 1U;
        constexpr std::uint64_t noOverflows   = 0U;
        constexpr std::size_t   emptyQueue    = 0U;

        [[nodiscard]]
        bool
        coalescible_motion( const Event& event ) noexcept
        {
            return event.kind == EventKind::MouseMove;
        }

    }    // namespace
}    // namespace grab

namespace grab::detail
{

    class SubscriptionState
    {
        public:

            SubscriptionState( EventFilter filter,
                               std::size_t max_queue ) :
                filter_( std::move( filter ) ),
                buffer_( max_queue )
            {
            }

            [[nodiscard]]
            bool
            matches( const Event& event ) const noexcept
            {
                return filter_.matches( event );
            }

            [[nodiscard]]
            std::function<void()>
            enqueue( const Event& event )
            {
                std::function<void()> notify;
                {
                    const std::scoped_lock lock( mutex_ );
                    if( buffer_.empty() )
                    {
                        mark_overflow();
                        return notify;
                    }

                    if( size_ < buffer_.size() )
                    {
                        buffer_.at( tail_index() ) = event;
                        ++size_;
                        notify = notify_;
                        return notify;
                    }

                    if( coalescible_motion( event ) &&
                        buffer_.at( back_index() ).kind == EventKind::MouseMove )
                    {
                        buffer_.at( back_index() ) = event;
                        notify                     = notify_;
                        return notify;
                    }

                    mark_overflow();
                }
                return notify;
            }

            [[nodiscard]]
            std::optional<Event>
            try_pop()
            {
                const std::scoped_lock lock( mutex_ );
                if( size_ == emptyQueue )
                {
                    return std::nullopt;
                }

                Event event         = std::move( buffer_.at( head_ ) );
                buffer_.at( head_ ) = Event{};
                head_               = next_index( head_ );
                --size_;
                return event;
            }

            [[nodiscard]]
            std::uint64_t
            overflow_count() const noexcept
            {
                return overflow_count_.load( std::memory_order_relaxed );
            }

            [[nodiscard]]
            bool
            lagging() const noexcept
            {
                return lagging_.load( std::memory_order_relaxed );
            }

            void
            set_notify( std::function<void()> on_data )
            {
                const std::scoped_lock lock( mutex_ );
                notify_ = std::move( on_data );
            }

        private:

            [[nodiscard]]
            std::size_t
            tail_index() const noexcept
            {
                return ( head_ + size_ ) % buffer_.size();
            }

            [[nodiscard]]
            std::size_t
            back_index() const noexcept
            {
                return ( head_ + size_ - 1U ) % buffer_.size();
            }

            [[nodiscard]]
            std::size_t
            next_index( std::size_t index ) const noexcept
            {
                return ( index + 1U ) % buffer_.size();
            }

            void
            mark_overflow() noexcept
            {
                lagging_.store( true, std::memory_order_relaxed );
                overflow_count_.fetch_add( 1U, std::memory_order_relaxed );
            }

            EventFilter                filter_;
            mutable std::mutex         mutex_;
            std::vector<Event>         buffer_;
            std::size_t                head_ = 0U;
            std::size_t                size_ = 0U;
            std::function<void()>      notify_;
            std::atomic<std::uint64_t> overflow_count_{ noOverflows };
            std::atomic_bool           lagging_{ false };
    };

    class EventBusState
    {
        public:

            void
            add( std::shared_ptr<SubscriptionState> subscription )
            {
                const std::scoped_lock lock( mutex_ );
                subscriptions_.push_back( std::move( subscription ) );
            }

            void
            remove( const std::shared_ptr<SubscriptionState>& subscription ) noexcept
            {
                try
                {
                    const std::scoped_lock lock( mutex_ );
                    const auto             first_removed =
                        std::ranges::remove( subscriptions_, subscription ).begin();
                    subscriptions_.erase( first_removed, subscriptions_.end() );
                }
                catch( ... )
                {
                    return;
                }
            }

            void
            publish( Event event ) noexcept
            {
                std::vector<std::function<void()>> to_notify;
                try
                {
                    const std::scoped_lock lock( mutex_ );
                    event.sequence = next_sequence_;
                    ++next_sequence_;
                    to_notify.reserve( subscriptions_.size() );
                    for( const auto& subscription : subscriptions_ )
                    {
                        if( !subscription->matches( event ) )
                        {
                            continue;
                        }

                        auto notify = subscription->enqueue( event );
                        if( notify )
                        {
                            to_notify.push_back( std::move( notify ) );
                        }
                    }
                }
                catch( ... )
                {
                    return;
                }

                for( const auto& notify : to_notify )
                {
                    try
                    {
                        notify();
                    }
                    catch( ... )
                    {
                        continue;
                    }
                }
            }

        private:

            std::mutex                                      mutex_;
            std::vector<std::shared_ptr<SubscriptionState>> subscriptions_;
            std::uint64_t next_sequence_ = firstSequence;
    };

}    // namespace grab::detail

namespace grab
{

    Subscription::Subscription() noexcept = default;

    Subscription::Subscription(
        std::weak_ptr<detail::EventBusState>       bus,
        std::shared_ptr<detail::SubscriptionState> state
    ) noexcept :
        bus_( std::move( bus ) ),
        state_( std::move( state ) )
    {
    }

    Subscription::~Subscription()
    {
        unsubscribe();
    }

    Subscription::Subscription( Subscription&& other ) noexcept :
        bus_( std::move( other.bus_ ) ),
        state_( std::move( other.state_ ) )
    {
    }

    Subscription&
    Subscription::operator=( Subscription&& other ) noexcept
    {
        if( this != &other )
        {
            unsubscribe();
            bus_   = std::move( other.bus_ );
            state_ = std::move( other.state_ );
        }
        return *this;
    }

    std::optional<Event>
    Subscription::try_pop()
    {
        if( state_ == nullptr )
        {
            return std::nullopt;
        }
        return state_->try_pop();
    }

    std::uint64_t
    Subscription::overflow_count() const noexcept
    {
        if( state_ == nullptr )
        {
            return noOverflows;
        }
        return state_->overflow_count();
    }

    bool
    Subscription::lagging() const noexcept
    {
        return state_ != nullptr && state_->lagging();
    }

    void
    Subscription::set_notify( std::function<void()> on_data )
    {
        if( state_ == nullptr )
        {
            return;
        }
        state_->set_notify( std::move( on_data ) );
    }

    void
    Subscription::unsubscribe() noexcept
    {
        auto state = std::move( state_ );
        auto bus   = bus_.lock();
        bus_.reset();
        if( bus == nullptr || state == nullptr )
        {
            return;
        }
        bus->remove( state );
    }

    EventBus::EventBus() :
        state_( std::make_shared<detail::EventBusState>() )
    {
    }

    EventBus::~EventBus() = default;

    void
    EventBus::publish( Event event ) noexcept
    {
        state_->publish( std::move( event ) );
    }

    Subscription
    EventBus::subscribe( EventFilter filter,
                         std::size_t max_queue )
    {
        auto subscription =
            std::make_shared<detail::SubscriptionState>( std::move( filter ),
                                                         max_queue );
        state_->add( subscription );
        return Subscription{ state_, std::move( subscription ) };
    }

}    // namespace grab
