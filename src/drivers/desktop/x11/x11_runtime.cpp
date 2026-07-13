#include "drivers/desktop/x11/x11_runtime.hpp"
#include "grab/context.hpp"
#include "grab/result.hpp"
#include "platform/x11/xcb_connection.hpp"
#include "spi/event_source.hpp"
#include "spi/route.hpp"
#include "spi/topology_source.hpp"
#include "spi/tree_source.hpp"

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <utility>

namespace grab::drivers::desktop::x11
{

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
        return {};
    }

    grab::Result<void>
    X11Runtime::stop()
    {
        connection_ = grab::platform::x11::XcbConnection{};
        return {};
    }

    grab::spi::TreeSource*
    X11Runtime::tree_source()
    {
        return nullptr;
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
