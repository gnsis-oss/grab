#include "drivers/desktop/x11/overlay_delegate.hpp"
#include "drivers/desktop/x11/x11_capture_route.hpp"
#include "drivers/desktop/x11/x11_event_source.hpp"
#include "drivers/desktop/x11/x11_routes.hpp"
#include "drivers/desktop/x11/x11_runtime.hpp"
#include "drivers/desktop/x11/x11_topology_source.hpp"
#include "drivers/desktop/x11/x11_tree_source.hpp"
#include "drivers/desktop/x11/x11_xtest_seat.hpp"
#include "drivers/desktop/x11/xcb_connection.hpp"
#include "drivers/desktop/x11/xkb_keymap.hpp"
#include "grab/capability.hpp"
#include "grab/context.hpp"
#include "grab/event.hpp"
#include "grab/ids.hpp"
#include "grab/result.hpp"
#include "kernel/graph/target_registry.hpp"
#include "spi/event_source.hpp"
#include "spi/overlay_delegate.hpp"
#include "spi/route.hpp"
#include "spi/topology_source.hpp"
#include "spi/tree_source.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

namespace grab::drivers::desktop::x11
{

    X11Runtime::X11Runtime( grab::core::Reactor* reactor ) noexcept :
        reactor_{ reactor }
    {
    }

    X11Runtime::~X11Runtime() = default;

    std::string_view
    X11Runtime::name() const
    {
        return "x11";
    }

    std::uint32_t
    X11Runtime::generation() const
    {
        return generation_;
    }

    grab::Result<void>
    X11Runtime::start( const grab::OperationContext& context )
    {
        const auto context_result = context.check();
        if( !context_result.has_value() )
        {
            return std::unexpected( context_result.error() );
        }

        if( connection_.get() != nullptr )
        {
            return {};
        }

        auto opened_connection = grab::platform::x11::XcbConnection::open( "" );
        if( !opened_connection.has_value() )
        {
            return std::unexpected( opened_connection.error() );
        }

        connection_      = std::move( *opened_connection );

        auto opened_seat = grab::input::Seat::open();
        if( !opened_seat.has_value() )
        {
            connection_ = grab::platform::x11::XcbConnection{};
            return std::unexpected( std::move( opened_seat.error() ) );
        }

        auto keymap = grab::platform::x11::make_keymap_from_connection( connection_ );
        if( !keymap.has_value() )
        {
            connection_ = grab::platform::x11::XcbConnection{};
            return std::unexpected( std::move( keymap.error() ) );
        }

        const auto next_generation = has_started_ ? generation_ + 1U : generation_;
        tree_source_ =
            std::make_unique<X11TreeSource>( grab::RuntimeId{ next_generation },
                                             grab::DisplayGeneration{ next_generation },
                                             targets_,
                                             connection_.get(),
                                             connection_.root() );
        input_seat_ =
            std::make_unique<X11InputSeat>( std::move( *opened_seat ), &ledger_ );
        pointer_route_  = std::make_unique<X11PointerRoute>( *tree_source_,
                                                             connection_.get(),
                                                             connection_.root(),
                                                             *input_seat_ );
        keyboard_route_ = std::make_unique<X11KeyboardRoute>( *tree_source_,
                                                              connection_.get(),
                                                              *input_seat_,
                                                              std::move( *keymap ) );

        auto opened_event_source =
            X11EventSource::open( connection_.get(), connection_.root(), ledger_ );
        if( !opened_event_source.has_value() )
        {
            keyboard_route_.reset();
            pointer_route_.reset();
            input_seat_.reset();
            tree_source_.reset();
            connection_ = grab::platform::x11::XcbConnection{};
            return std::unexpected( std::move( opened_event_source.error() ) );
        }

        event_source_ = std::move( *opened_event_source );
        if( pending_sink_ )
        {
            event_source_->set_sink( pending_sink_ );
        }
        activation_route_ = std::make_unique<X11ActivationRoute>( *tree_source_,
                                                                  connection_.get(),
                                                                  connection_.root(),
                                                                  *event_source_ );

        // Same display authority the runtime connects to (DISPLAY env for now);
        // explicit display threading through the runtime is Task 8 scope.
        capture_route_.reset();
        capture_route_error_.reset();
        auto capture_route = X11CaptureRoute::open();
        if( capture_route.has_value() )
        {
            capture_route_.emplace( std::move( *capture_route ) );
        }
        else
        {
            capture_route_error_ = std::move( capture_route.error() );
        }
        topology_source_ = std::make_unique<X11TopologySource>(
            [this]
            {
                if( capture_route_.has_value() )
                {
                    // Best-effort: topology changes refresh the capture authority.
                    static_cast<void>(
                        capture_route_->refresh_transforms()
                    );    // NOLINT(bugprone-unused-return-value)
                }
            }
        );
        auto overlay_probe = X11OverlayDelegate::probe();
        overlay_available_ = overlay_probe.has_value();
        overlay_delegate_error_.reset();
        if( !overlay_probe.has_value() )
        {
            overlay_delegate_error_ = std::move( overlay_probe.error() );
        }
        generation_  = next_generation;
        has_started_ = true;
        return {};
    }

    grab::Result<void>
    X11Runtime::stop()
    {
        if( overlay_delegate_ != nullptr )
        {
            overlay_delegate_->close();
        }
        overlay_delegate_.reset();
        overlay_delegate_error_.reset();
        overlay_available_ = false;
        topology_source_.reset();
        activation_route_.reset();
        event_source_.reset();
        capture_route_.reset();
        capture_route_error_.reset();
        keyboard_route_.reset();
        pointer_route_.reset();
        input_seat_.reset();
        tree_source_.reset();
        connection_ = grab::platform::x11::XcbConnection{};
        return {};
    }

    X11CaptureRoute*
    X11Runtime::capture_route() noexcept
    {
        return capture_route_.has_value() ? &*capture_route_ : nullptr;
    }

    const grab::Error*
    X11Runtime::capture_route_error() const noexcept
    {
        return capture_route_error_.has_value() ? &*capture_route_error_ : nullptr;
    }

    void
    X11Runtime::set_event_sink( std::function<void( grab::Event&& )> sink )
    {
        pending_sink_ = std::move( sink );
        if( event_source_ != nullptr )
        {
            event_source_->set_sink( pending_sink_ );
        }
    }

    X11InputSeat*
    X11Runtime::native_seat() noexcept
    {
        return input_seat_.get();
    }

    grab::spi::TreeSource*
    X11Runtime::tree_source()
    {
        return tree_source_.get();
    }

    grab::Result<std::uint32_t>
    X11Runtime::resolve_native_window( const grab::WidgetRef& widget ) const
    {
        if( tree_source_ == nullptr )
        {
            return grab::fail( grab::ErrorCode::CapabilityUnavailable,
                               "X11 runtime has no tree source" );
        }
        return tree_source_->resolve_xid( widget );
    }

    grab::kernel::TargetRegistry*
    X11Runtime::target_registry() noexcept
    {
        return &targets_;
    }

    const grab::kernel::TargetRegistry*
    X11Runtime::target_registry() const noexcept
    {
        return &targets_;
    }

    grab::spi::TopologySource*
    X11Runtime::topology_source()
    {
        return topology_source_.get();
    }

    grab::spi::EventSource*
    X11Runtime::event_source()
    {
        return event_source_.get();
    }

    grab::spi::OverlayDelegate*
    X11Runtime::overlay_delegate()
    {
        if( connection_.get() == nullptr )
        {
            return nullptr;
        }
        if( overlay_delegate_ == nullptr )
        {
            auto created = X11OverlayDelegate::create( reactor_ );
            if( !created.has_value() )
            {
                overlay_available_      = false;
                overlay_delegate_error_ = std::move( created.error() );
                return nullptr;
            }
            overlay_delegate_ = std::move( *created );
            overlay_delegate_->set_availability_changed(
                [this]( bool available, const grab::Error* error )
                {
                    overlay_available_ = available;
                    if( error != nullptr )
                    {
                        overlay_delegate_error_ = *error;
                    }
                    else if( available )
                    {
                        overlay_delegate_error_.reset();
                    }
                }
            );
            overlay_delegate_->set_topology_refresh(
                [this]() -> grab::Result<void>
                {
                    if( !capture_route_.has_value() )
                    {
                        return grab::fail(
                            grab::ErrorCode::CapabilityUnavailable,
                            "X11 overlay topology has no coordinate authority"
                        );
                    }
                    auto refreshed = capture_route_->force_refresh_transforms();
                    if( !refreshed.has_value() )
                    {
                        return std::unexpected( std::move( refreshed.error() ) );
                    }
                    return {};
                }
            );
        }
        return overlay_delegate_.get();
    }

    std::span<const grab::Capability>
    X11Runtime::capabilities() const noexcept
    {
        return x11_capability_rows( overlay_available_ );
    }

    const grab::Error*
    X11Runtime::overlay_delegate_error() const noexcept
    {
        return overlay_delegate_error_.has_value() ? &*overlay_delegate_error_ : nullptr;
    }

    std::span<const grab::spi::RouteDescriptor>
    X11Runtime::routes() const
    {
        return x11_route_descriptors();
    }

    grab::spi::ActionRoute*
    X11Runtime::action_route( std::size_t index )
    {
        if( index == 0U )
        {
            return pointer_route_.get();
        }
        if( index == 1U )
        {
            return keyboard_route_.get();
        }
        if( index == 2U )
        {
            return nullptr;
        }
        if( index == 3U )
        {
            return activation_route_.get();
        }
        return nullptr;
    }

    grab::spi::InputSeat*
    X11Runtime::input_seat()
    {
        return input_seat_.get();
    }

}    // namespace grab::drivers::desktop::x11
