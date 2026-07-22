#pragma once

#include <cstdlib>
#include <memory>

namespace grab::platform::x11
{

    template<typename T>
    using XcbReply = std::unique_ptr<T, void ( * )( void* )>;

    template<typename T>
    [[nodiscard]]
    XcbReply<T>
    make_xcb_reply( T* reply ) noexcept
    {
        return XcbReply<T>{ reply, &std::free };
    }

}    // namespace grab::platform::x11
