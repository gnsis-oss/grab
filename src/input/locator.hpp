#pragma once

#include "grab/geometry/rectangle.hpp"
#include "grab/result.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct xcb_connection_t;

namespace grab::input
{

    enum class GeometryTrust : std::uint8_t
    {
        Trusted,
        Estimated,
        Unavailable,
    };

    struct LocatedWindow
    {
            std::uint32_t             window = 0U;
            grab::geometry::Rectangle bounds;
            GeometryTrust             trust = GeometryTrust::Unavailable;
    };

    class WindowLocator
    {
        public:

            [[nodiscard]]
            static grab::Result<WindowLocator>
            open( const char* display = nullptr );

            ~WindowLocator();

            WindowLocator( const WindowLocator& ) = delete;
            WindowLocator&
            operator=( const WindowLocator& ) = delete;
            WindowLocator( WindowLocator&& other ) noexcept;
            WindowLocator&
            operator=( WindowLocator&& other ) noexcept;

            [[nodiscard]]
            grab::Result<LocatedWindow>
            locate( const std::vector<std::string>& wm_class_candidates,
                    std::string_view                title = {} );

            [[nodiscard]]
            grab::Result<void>
            activate( const LocatedWindow& window );

        private:

            WindowLocator( xcb_connection_t* connection,
                           std::uint32_t     root ) noexcept;

            xcb_connection_t* connection_ = nullptr;
            std::uint32_t     root_       = 0U;
    };

}    // namespace grab::input
