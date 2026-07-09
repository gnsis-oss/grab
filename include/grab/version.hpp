#ifndef GRAB_VERSION_HPP
#define GRAB_VERSION_HPP

#include <string_view>

namespace grab
{

    [[nodiscard]]
    std::string_view
    version() noexcept;

}    // namespace grab

#endif
