#include "drivers/desktop/x11/x11_capture_route.hpp"
#include "drivers/desktop/x11/x11_routes.hpp"
#include "drivers/desktop/x11/x11_runtime.hpp"
#include "drivers/desktop/x11/x11_tree_source.hpp"
#include "grab/context.hpp"
#include "grab/ids.hpp"
#include "grab/result.hpp"
#include "input/seat.hpp"
#include "kernel/graph/target_registry.hpp"
#include "platform/x11/xcb_connection.hpp"
#include "platform/x11/xkb_keymap.hpp"
#include "spi/event_source.hpp"
#include "spi/route.hpp"
#include "spi/topology_source.hpp"
#include "spi/tree_source.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

namespace grab::drivers::desktop::x11
{

    X11Runtime::X11Runtime()  = default;

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
        input_seat_     = std::make_unique<X11InputSeat>( std::move( *opened_seat ) );
        pointer_route_  = std::make_unique<X11PointerRoute>( *tree_source_,
                                                             connection_.get(),
                                                             connection_.root(),
                                                             *input_seat_ );
        keyboard_route_ = std::make_unique<X11KeyboardRoute>( *tree_source_,
                                                              connection_.get(),
                                                              *input_seat_,
                                                              std::move( *keymap ) );

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
        generation_  = next_generation;
        has_started_ = true;
        return {};
    }

    grab::Result<void>
    X11Runtime::stop()
    {
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
        return nullptr;
    }

    grab::spi::EventSource*
    X11Runtime::event_source()
    {
        return nullptr;
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
        return nullptr;
    }

    grab::spi::InputSeat*
    X11Runtime::input_seat()
    {
        return input_seat_.get();
    }

}    // namespace grab::drivers::desktop::x11
