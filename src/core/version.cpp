#include "grab/version.hpp"

#include <string_view>

namespace grab
{

    namespace
    {

        constexpr std::string_view kVersion = "0.1.0-dev";

    }    // namespace

    std::string_view
    version() noexcept
    {
        return kVersion;
    }

}    // namespace grab
