#include "drivers/semantic/atspi/atspi_monitor.hpp"
#include "drivers/semantic/atspi/atspi_runtime.hpp"
#include "drivers/semantic/atspi/atspi_tree_source.hpp"
#include "grab/context.hpp"
#include "grab/event.hpp"
#include "grab/event_descriptor.hpp"
#include "grab/ids.hpp"
#include "grab/result.hpp"
#include "kernel/events/event_bus.hpp"
#include "kernel/graph/target_registry.hpp"
#include "spi/event_source.hpp"
#include "spi/route.hpp"
#include "spi/tree_source.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace grab::drivers::semantic::atspi
{
    namespace
    {

        [[nodiscard]]
        std::vector<grab::EventKind>
        accessibility_kinds()
        {
            std::vector<grab::EventKind> kinds;
            kinds.reserve( grab::detail::eventDescriptors.size() );
            for( const auto& descriptor : grab::detail::eventDescriptors )
            {
                if( descriptor.category == grab::EventCategory::Accessibility )
                {
                    kinds.push_back( descriptor.kind );
                }
            }
            return kinds;
        }

        constexpr auto routeDescriptors = std::to_array<grab::spi::RouteDescriptor>( {
            {
             .name          = "atspi.invoke",
             .kind          = grab::spi::RouteKind::Semantic,
             .fidelity      = grab::spi::RouteFidelity::Exact,
             .latency_class = grab::spi::RouteLatencyClass::Immediate,
             },
            {
             .name          = "atspi.set-value",
             .kind          = grab::spi::RouteKind::Semantic,
             .fidelity      = grab::spi::RouteFidelity::Exact,
             .latency_class = grab::spi::RouteLatencyClass::Immediate,
             },
            {
             .name          = "atspi.set-text",
             .kind          = grab::spi::RouteKind::Semantic,
             .fidelity      = grab::spi::RouteFidelity::Exact,
             .latency_class = grab::spi::RouteLatencyClass::Immediate,
             },
            {
             .name          = "atspi.select",
             .kind          = grab::spi::RouteKind::Semantic,
             .fidelity      = grab::spi::RouteFidelity::Exact,
             .latency_class = grab::spi::RouteLatencyClass::Immediate,
             },
        } );

    }    // namespace

    class AtspiEventSource final : public grab::spi::EventSource
    {
        public:

            explicit AtspiEventSource( grab::EventBus& bus ) :
                subscription_( bus.subscribe( grab::EventFilter{
                    // EventBus::subscribe expands empty kinds to every kind. For
                    // this long-lived source that would saturate per-kind demand
                    // refcounts and suppress XI2 mask-arming transitions for
                    // unrelated input subscribers.
                    .kinds      = accessibility_kinds(),
                    .categories = { grab::EventCategory::Accessibility },
                } ) )
            {
                subscription_.set_notify(
                    [this]()
                    {
                        {
                            const std::scoped_lock lock{ mutex_ };
                            notified_ = true;
                        }
                        condition_.notify_all();
                    }
                );
            }

            ~AtspiEventSource() override
            {
                subscription_.set_notify( {} );
            }

            AtspiEventSource( const AtspiEventSource& ) = delete;
            AtspiEventSource&
            operator=( const AtspiEventSource& )   = delete;
            AtspiEventSource( AtspiEventSource&& ) = delete;
            AtspiEventSource&
            operator=( AtspiEventSource&& ) = delete;

            [[nodiscard]]
            grab::Result<void>
            enable( const grab::spi::EventSpec& spec ) override
            {
                if( spec.name.empty() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "AT-SPI event spec must not be empty" );
                }
                bool                        became_non_empty = false;
                std::function<void( bool )> demand_sink;
                {
                    const std::scoped_lock lock{ mutex_ };
                    const bool             was_empty = enabled_.empty();
                    enabled_.insert( spec.name );
                    became_non_empty = was_empty && !enabled_.empty();
                    if( became_non_empty )
                    {
                        demand_sink = demand_sink_;
                    }
                }
                if( became_non_empty && demand_sink )
                {
                    demand_sink( true );
                }
                return {};
            }

            [[nodiscard]]
            grab::Result<void>
            disable( const grab::spi::EventSpec& spec ) override
            {
                bool                        became_empty = false;
                std::function<void( bool )> demand_sink;
                {
                    const std::scoped_lock lock{ mutex_ };
                    const bool             was_non_empty = !enabled_.empty();
                    enabled_.erase( spec.name );
                    became_empty = was_non_empty && enabled_.empty();
                    if( became_empty )
                    {
                        demand_sink = demand_sink_;
                    }
                }
                if( became_empty && demand_sink )
                {
                    demand_sink( false );
                }
                return {};
            }

            void
            set_demand_sink( std::function<void( bool )> sink )
            {
                const std::scoped_lock lock{ mutex_ };
                demand_sink_ = std::move( sink );
            }

            [[nodiscard]]
            grab::Result<void>
            wait_for_event( const grab::spi::EventSpec&   spec,
                            const grab::OperationContext& context,
                            std::chrono::nanoseconds      maximum_wait ) override
            {
                if( maximum_wait < std::chrono::nanoseconds::zero() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "AT-SPI event wait must not be negative" );
                }
                {
                    const std::scoped_lock lock{ mutex_ };
                    if( !enabled_.contains( spec.name ) )
                    {
                        return grab::fail( grab::ErrorCode::SubscriptionGone,
                                           "AT-SPI event spec is not enabled" );
                    }
                }

                const auto started = std::chrono::steady_clock::now();
                while( std::chrono::steady_clock::now() - started < maximum_wait )
                {
                    const auto checked = context.check();
                    if( !checked.has_value() )
                    {
                        return std::unexpected( checked.error() );
                    }
                    if( subscription_.try_pop_item().has_value() )
                    {
                        return {};
                    }

                    const auto elapsed   = std::chrono::steady_clock::now() - started;
                    const auto remaining = maximum_wait - elapsed;
                    const auto slice =
                        std::min( remaining,
                                  std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::milliseconds{ 10 }
                                  ) );
                    std::unique_lock lock{ mutex_ };
                    condition_.wait_for( lock,
                                         slice,
                                         [this]()
                                         {
                                             return notified_;
                                         } );
                    notified_ = false;
                }
                return {};
            }

        private:

            grab::Subscription          subscription_;
            std::mutex                  mutex_;
            std::condition_variable     condition_;
            std::set<std::string>       enabled_;
            std::function<void( bool )> demand_sink_;
            bool                        notified_{};
    };

    AtspiRuntime::AtspiRuntime() :
        owned_targets_( std::make_unique<grab::kernel::TargetRegistry>() ),
        targets_( owned_targets_.get() )
    {
    }

    AtspiRuntime::AtspiRuntime(
        grab::core::Reactor&                  reactor,
        grab::EventBus&                       event_bus,
        AtspiTreeSource::AccessibleEnumerator enumerate_accessibles,
        std::optional<std::string>            x11_alias_authority
    ) :
        reactor_( &reactor ),
        event_bus_( &event_bus ),
        owned_targets_( std::make_unique<grab::kernel::TargetRegistry>() ),
        targets_( owned_targets_.get() ),
        enumerate_accessibles_( std::move( enumerate_accessibles ) ),
        x11_alias_authority_( std::move( x11_alias_authority ) )
    {
    }

    AtspiRuntime::AtspiRuntime(
        grab::core::Reactor&                  reactor,
        grab::EventBus&                       event_bus,
        grab::kernel::TargetRegistry&         targets,
        AtspiTreeSource::AccessibleEnumerator enumerate_accessibles,
        std::optional<std::string>            x11_alias_authority,
        grab::RuntimeId                       initial_runtime_id
    ) :
        reactor_( &reactor ),
        event_bus_( &event_bus ),
        targets_( &targets ),
        enumerate_accessibles_( std::move( enumerate_accessibles ) ),
        x11_alias_authority_( std::move( x11_alias_authority ) ),
        runtime_id_( initial_runtime_id )
    {
    }

    AtspiRuntime::~AtspiRuntime()
    {
        static_cast<void>( stop() );    // NOLINT(bugprone-unused-return-value)
    }

    std::string_view
    AtspiRuntime::name() const
    {
        return "atspi";
    }

    std::uint32_t
    AtspiRuntime::generation() const
    {
        return generation_;
    }

    grab::Result<void>
    AtspiRuntime::start( const grab::OperationContext& context )
    {
        const auto checked = context.check();
        if( !checked.has_value() )
        {
            return std::unexpected( checked.error() );
        }
        if( monitor_ != nullptr )
        {
            return {};
        }
        if( reactor_ == nullptr || event_bus_ == nullptr || targets_ == nullptr )
        {
            return grab::fail( grab::ErrorCode::CapabilityUnavailable,
                               "AT-SPI runtime needs a reactor and event bus" );
        }

        event_source_ = std::make_unique<AtspiEventSource>( *event_bus_ );
        auto monitor  = grab::event::AtspiMonitor::start( *reactor_, *event_bus_ );
        if( !monitor.has_value() )
        {
            event_source_.reset();
            return std::unexpected( std::move( monitor.error() ) );
        }

        const auto next_generation = has_started_ ? generation_ + 1U : generation_;
        const auto next_runtime_id =
            has_started_ ? grab::RuntimeId{ runtime_id_.value + 1U } : runtime_id_;
        tree_source_ = std::make_unique<AtspiTreeSource>( next_runtime_id,
                                                          *targets_,
                                                          enumerate_accessibles_,
                                                          x11_alias_authority_ );
        monitor_ = std::make_unique<grab::event::AtspiMonitor>( std::move( *monitor ) );
        event_source_->set_demand_sink(
            [this]( bool enabled )
            {
                if( monitor_ == nullptr )
                {
                    return;
                }
                if( enabled )
                {
                    // NOLINTNEXTLINE(bugprone-unused-return-value)
                    static_cast<void>( monitor_->enable_events() );
                }
                else
                {
                    monitor_->disable_events();
                }
            }
        );
        generation_  = next_generation;
        runtime_id_  = next_runtime_id;
        has_started_ = true;
        return {};
    }

    grab::Result<void>
    AtspiRuntime::stop()
    {
        if( monitor_ != nullptr )
        {
            monitor_->stop();
        }
        monitor_.reset();
        event_source_.reset();
        tree_source_.reset();
        return {};
    }

    grab::spi::TreeSource*
    AtspiRuntime::tree_source()
    {
        return tree_source_.get();
    }

    grab::spi::EventSource*
    AtspiRuntime::event_source()
    {
        return event_source_.get();
    }

    std::span<const grab::spi::RouteDescriptor>
    AtspiRuntime::routes() const
    {
        return routeDescriptors;
    }

    grab::spi::ActionRoute*
    AtspiRuntime::action_route( [[maybe_unused]] std::size_t index )
    {
        // P1.9 advertises the semantic routes. Reservations are implemented
        // when the action request vocabulary grows Invoke/SetValue verbs.
        return nullptr;
    }

}    // namespace grab::drivers::semantic::atspi
