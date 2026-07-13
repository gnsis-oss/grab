#include "drivers/desktop/x11/x11_runtime.hpp"
#include "drivers/desktop/x11/x11_tree_source.hpp"
#include "grab/context.hpp"
#include "grab/ids.hpp"
#include "grab/result.hpp"
#include "kernel/graph/target_registry.hpp"
#include "platform/x11/xcb_connection.hpp"
#include "spi/event_source.hpp"
#include "spi/route.hpp"
#include "spi/topology_source.hpp"
#include "spi/tree_source.hpp"

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

        connection_ = std::move( *opened_connection );
        if( has_started_ )
        {
            ++generation_;
        }
        has_started_ = true;
        tree_source_ =
            std::make_unique<X11TreeSource>( grab::RuntimeId{ generation_ },
                                             grab::DisplayGeneration{ generation_ },
                                             connection_.get(),
                                             connection_.root() );
        return {};
    }

    grab::Result<void>
    X11Runtime::stop()
    {
        tree_source_.reset();
        connection_ = grab::platform::x11::XcbConnection{};
        return {};
    }

    grab::spi::TreeSource*
    X11Runtime::tree_source()
    {
        return tree_source_.get();
    }

    const grab::kernel::TargetRegistry*
    X11Runtime::target_registry() const noexcept
    {
        if( tree_source_ == nullptr )
        {
            return nullptr;
        }
        return &tree_source_->target_registry();
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
        return {};
    }

}    // namespace grab::drivers::desktop::x11
