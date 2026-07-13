#pragma once
// grab shim: satisfies `#include <log/writer.hpp>` used by vendored l0 headers.
// grab does not adopt l0 logging; every symbol here is a no-op. The surface is
// exactly the `logger::` free functions the copied trees call.
#include <string_view>

namespace logger
{

    struct Tag
    {
            std::string_view name;
    };

    [[nodiscard]]
    constexpr Tag
    tag( std::string_view name ) noexcept
    {
        return Tag{ name };
    }

    template<typename... Args>
    constexpr void
    trace( Tag,
           Args&&... ) noexcept
    {
    }

    template<typename... Args>
    constexpr void
    error( Tag,
           Args&&... ) noexcept
    {
    }

    template<typename... Args>
    constexpr void
    nominal( Tag,
             Args&&... ) noexcept
    {
    }

    template<typename... Args>
    constexpr void
    verbose( Tag,
             Args&&... ) noexcept
    {
    }

    template<typename... Args>
    constexpr void
    debug( Tag,
           Args&&... ) noexcept
    {
    }

}
