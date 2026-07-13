#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/event_descriptor.hpp"
#include "grab/ids.hpp"
#include "grab/result.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace grab
{
    namespace
    {

        constexpr std::uint64_t firstSequence = 1U;
        constexpr std::uint64_t noOverflows   = 0U;
        constexpr std::size_t   emptyQueue    = 0U;

        [[nodiscard]]
        SubscriptionId
        make_subscription_id() noexcept
        {
            constexpr std::size_t             uuidSize       = 16U;
            constexpr std::size_t             counterBytes   = sizeof( std::uint64_t );
            constexpr std::size_t             versionByte    = 6U;
            constexpr std::size_t             variantByte    = 8U;
            constexpr std::uint8_t            versionSeven   = 0X70U;
            constexpr std::uint8_t            variantRfc9562 = 0X80U;
            constexpr std::uint64_t           byteMask       = 0XFFU;
            constexpr std::size_t             bitsPerByte    = 8U;

            static std::atomic<std::uint64_t> nextSubscriptionId{ 1U };
            Uuid                              uuid{};
            const auto                        value =
                nextSubscriptionId.fetch_add( 1U, std::memory_order_relaxed );
            for( std::size_t offset = 0U; offset < counterBytes; ++offset )
            {
                const auto shift = offset * bitsPerByte;
                uuid.bytes.at( uuidSize - 1U - offset ) =
                    static_cast<std::uint8_t>( ( value >> shift ) & byteMask );
            }
            uuid.bytes.at( versionByte ) = versionSeven;
            uuid.bytes.at( variantByte ) = variantRfc9562;
            return SubscriptionId{ .value = uuid };
        }

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

            SubscriptionState( SubscriptionId    id,
                               SubscriptionScope scope,
                               QueueOptions      options ) :
                id_( id ),
                scope_( std::move( scope ) ),
                buffer_( options.capacity ),
                overflow_policy_( options.overflow )
            {
            }

            [[nodiscard]]
            SubscriptionId
            id() const noexcept
            {
                return id_;
            }

            [[nodiscard]]
            SubscriptionScope
            scope() const
            {
                return scope_;
            }

            [[nodiscard]]
            const std::vector<EventKind>&
            kinds() const noexcept
            {
                return scope_.kinds;
            }

            [[nodiscard]]
            bool
            matches( const Event& event ) const noexcept
            {
                const auto& kinds = scope_.kinds;
                return ( kinds.empty() ||
                         std::ranges::find( kinds, event.kind ) != kinds.end() ) &&
                       scope_.filter.matches( event );
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
                        buffer_.at( tail_index() ) = SubscriptionEvent{ event };
                        ++size_;
                        notify = notify_;
                        return notify;
                    }

                    if( overflow_policy_ ==
                        QueueOverflowPolicy::Coalesce &&
                        coalescing_class_of( event.kind ) ==
                        CoalescingClass::Coalesce &&
                        coalescible_motion( event ) &&
                        std::holds_alternative<Event>( buffer_.at( back_index() ) ) &&
                        std::get<Event>( buffer_.at( back_index() ) ).kind ==
                        EventKind::MouseMove )
                    {
                        buffer_.at( back_index() ) = SubscriptionEvent{ event };
                        notify                     = notify_;
                        return notify;
                    }

                    if( overflow_policy_ ==
                        QueueOverflowPolicy::NeverDrop ||
                        coalescing_class_of( event.kind ) == CoalescingClass::NeverDrop )
                    {
                        mark_gap();
                        notify = notify_;
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
                auto item = try_pop_item();
                if( item == std::nullopt || !std::holds_alternative<Event>( *item ) )
                {
                    return std::nullopt;
                }
                return std::get<Event>( std::move( *item ) );
            }

            [[nodiscard]]
            std::optional<SubscriptionEvent>
            try_pop_item()
            {
                const std::scoped_lock lock( mutex_ );
                if( size_ == emptyQueue )
                {
                    return std::nullopt;
                }

                auto item           = std::move( buffer_.at( head_ ) );
                buffer_.at( head_ ) = SubscriptionEvent{ Event{} };
                head_               = next_index( head_ );
                --size_;
                if( const auto* event = std::get_if<Event>( &item ) )
                {
                    last_delivered_sequence_ = event->sequence;
                }
                return item;
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

            [[nodiscard]]
            bool
            needs_resync() const noexcept
            {
                return needs_resync_.load( std::memory_order_relaxed );
            }

            [[nodiscard]]
            std::uint64_t
            dropped_count() const noexcept
            {
                return overflow_count();
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

            void
            mark_gap() noexcept
            {
                mark_overflow();
                needs_resync_.store( true, std::memory_order_relaxed );
                if( buffer_.empty() )
                {
                    return;
                }

                auto& back = buffer_.at( back_index() );
                if( std::holds_alternative<QueueGapMarker>( back ) )
                {
                    return;
                }
                back = QueueGapMarker{
                    .code                    = ErrorCode::QueueGap,
                    .last_delivered_sequence = last_delivered_sequence_,
                };
            }

            SubscriptionId                 id_;
            SubscriptionScope              scope_;
            mutable std::mutex             mutex_;
            std::vector<SubscriptionEvent> buffer_;
            QueueOverflowPolicy            overflow_policy_;
            std::size_t                    head_ = 0U;
            std::size_t                    size_ = 0U;
            std::function<void()>          notify_;
            std::atomic<std::uint64_t>     overflow_count_{ noOverflows };
            std::atomic_bool               lagging_{ false };
            std::atomic_bool               needs_resync_{ false };
            std::uint64_t                  last_delivered_sequence_ = 0U;
    };

    class EventBusState
    {
        public:

            void
            add( std::shared_ptr<SubscriptionState> subscription )
            {
                std::vector<std::pair<EventKind, bool>> transitions;
                EventBus::DemandCallback                callback;
                {
                    const std::scoped_lock lock( mutex_ );
                    for( const auto kind : subscription->kinds() )
                    {
                        if( replay_policy_of( kind ) != ReplayPolicy::CurrentSet )
                        {
                            continue;
                        }
                        const auto provider = snapshot_providers_.find( kind );
                        if( provider == snapshot_providers_.end() )
                        {
                            continue;
                        }
                        for( auto event : provider->second() )
                        {
                            if( event.kind != kind || !subscription->matches( event ) )
                            {
                                continue;
                            }
                            event.sequence = next_sequence_;
                            ++next_sequence_;
                            [[maybe_unused]]
                            auto notify = subscription->enqueue( event );
                        }
                    }

                    subscriptions_.push_back( subscription );
                    for( const auto kind : subscription->kinds() )
                    {
                        auto& count = subscription_refcounts_[kind];
                        if( count == 0U )
                        {
                            transitions.emplace_back( kind, true );
                        }
                        ++count;
                    }
                    callback = demand_callback_;
                }
                invoke_demand_callback( callback, transitions );
            }

            void
            remove( const std::shared_ptr<SubscriptionState>& subscription ) noexcept
            {
                std::vector<std::pair<EventKind, bool>> transitions;
                EventBus::DemandCallback                callback;
                try
                {
                    const std::scoped_lock lock( mutex_ );
                    const auto found = std::ranges::find( subscriptions_, subscription );
                    if( found == subscriptions_.end() )
                    {
                        return;
                    }
                    subscriptions_.erase( found );
                    for( const auto kind : subscription->kinds() )
                    {
                        const auto count = subscription_refcounts_.find( kind );
                        if( count == subscription_refcounts_.end() )
                        {
                            continue;
                        }
                        --count->second;
                        if( count->second == 0U )
                        {
                            transitions.emplace_back( kind, false );
                            subscription_refcounts_.erase( count );
                        }
                    }
                    callback = demand_callback_;
                }
                catch( ... )
                {
                    return;
                }
                invoke_demand_callback( callback, transitions );
            }

            void
            register_snapshot_provider( EventKind                  kind,
                                        EventBus::SnapshotProvider provider )
            {
                const std::scoped_lock lock( mutex_ );
                snapshot_providers_.insert_or_assign( kind, std::move( provider ) );
            }

            void
            unregister_snapshot_provider( EventKind kind )
            {
                const std::scoped_lock lock( mutex_ );
                snapshot_providers_.erase( kind );
            }

            void
            set_demand_callback( EventBus::DemandCallback callback )
            {
                const std::scoped_lock lock( mutex_ );
                demand_callback_ = std::move( callback );
            }

            [[nodiscard]]
            std::size_t
            subscription_refcount( EventKind kind ) const noexcept
            {
                try
                {
                    const std::scoped_lock lock( mutex_ );
                    const auto             count = subscription_refcounts_.find( kind );
                    return count == subscription_refcounts_.end() ? 0U : count->second;
                }
                catch( ... )
                {
                    return 0U;
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

            using SnapshotProvider = EventBus::SnapshotProvider;
            using DemandCallback   = EventBus::DemandCallback;

            static void
            invoke_demand_callback(
                const DemandCallback&               callback,
                const std::vector<std::pair<EventKind,
                                            bool>>& transitions
            ) noexcept
            {
                if( !callback )
                {
                    return;
                }
                for( const auto& [kind, enabled] : transitions )
                {
                    try
                    {
                        callback( kind, enabled );
                    }
                    catch( ... )
                    {
                        continue;
                    }
                }
            }

            mutable std::mutex                              mutex_;
            std::vector<std::shared_ptr<SubscriptionState>> subscriptions_;
            std::map<EventKind, SnapshotProvider>           snapshot_providers_;
            std::map<EventKind, std::size_t>                subscription_refcounts_;
            DemandCallback                                  demand_callback_;
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

    std::optional<SubscriptionEvent>
    Subscription::try_pop_item()
    {
        if( state_ == nullptr )
        {
            return std::nullopt;
        }
        return state_->try_pop_item();
    }

    SubscriptionId
    Subscription::id() const noexcept
    {
        if( state_ == nullptr )
        {
            return {};
        }
        return state_->id();
    }

    SubscriptionScope
    Subscription::scope() const
    {
        if( state_ == nullptr )
        {
            return {};
        }
        return state_->scope();
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

    bool
    Subscription::needs_resync() const noexcept
    {
        return state_ != nullptr && state_->needs_resync();
    }

    std::uint64_t
    Subscription::dropped_count() const noexcept
    {
        if( state_ == nullptr )
        {
            return noOverflows;
        }
        return state_->dropped_count();
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
    EventBus::subscribe( EventFilter  filter,
                         QueueOptions options )
    {
        auto kinds = std::vector<EventKind>( filter.kinds.begin(), filter.kinds.end() );
        return subscribe(
            SubscriptionScope{
                .kinds  = std::move( kinds ),
                .filter = std::move( filter ),
            },
            options
        );
    }

    Subscription
    EventBus::subscribe( SubscriptionScope scope,
                         QueueOptions      options )
    {
        if( scope.kinds.empty() )
        {
            scope.kinds.reserve( detail::eventDescriptors.size() );
            for( const auto& descriptor : detail::eventDescriptors )
            {
                scope.kinds.push_back( descriptor.kind );
            }
        }
        else
        {
            std::vector<EventKind> unique_kinds;
            unique_kinds.reserve( scope.kinds.size() );
            for( const auto kind : scope.kinds )
            {
                if( std::ranges::find( unique_kinds, kind ) == unique_kinds.end() )
                {
                    unique_kinds.push_back( kind );
                }
            }
            scope.kinds = std::move( unique_kinds );
        }

        auto subscription =
            std::make_shared<detail::SubscriptionState>( make_subscription_id(),
                                                         std::move( scope ),
                                                         options );
        state_->add( subscription );
        return Subscription{ state_, std::move( subscription ) };
    }

    Subscription
    EventBus::subscribe( EventFilter filter,
                         std::size_t max_queue )
    {
        return subscribe( std::move( filter ), QueueOptions{ .capacity = max_queue } );
    }

    void
    EventBus::register_snapshot_provider( EventKind        kind,
                                          SnapshotProvider provider )
    {
        state_->register_snapshot_provider( kind, std::move( provider ) );
    }

    void
    EventBus::unregister_snapshot_provider( EventKind kind )
    {
        state_->unregister_snapshot_provider( kind );
    }

    void
    EventBus::set_demand_callback( DemandCallback callback )
    {
        state_->set_demand_callback( std::move( callback ) );
    }

    std::size_t
    EventBus::subscription_refcount( EventKind kind ) const noexcept
    {
        return state_->subscription_refcount( kind );
    }

}    // namespace grab
