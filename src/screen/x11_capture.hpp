#ifndef GRAB_SCREEN_X11_CAPTURE_HPP
#define GRAB_SCREEN_X11_CAPTURE_HPP

#include "grab/image.hpp"
#include "grab/result.hpp"

#include <cstdint>
#include <vector>

struct xcb_connection_t;

namespace grab::screen
{

    class X11Capturer
    {
        public:

            [[nodiscard]]
            static grab::Result<X11Capturer>
            open( const char* display = nullptr );

            ~X11Capturer();

            X11Capturer( const X11Capturer& ) = delete;
            X11Capturer&
            operator=( const X11Capturer& ) = delete;
            X11Capturer( X11Capturer&& other ) noexcept;
            X11Capturer&
            operator=( X11Capturer&& other ) noexcept;

            [[nodiscard]]
            grab::Result<grab::Image>
            capture_window( std::uint32_t window );

            [[nodiscard]]
            grab::Result<grab::Image>
            capture_display();

            [[nodiscard]]
            grab::Result<grab::Image>
            capture_region( std::int16_t  x,
                            std::int16_t  y,
                            std::uint16_t width,
                            std::uint16_t height );

        private:

            X11Capturer( xcb_connection_t* connection,
                         std::uint32_t     root,
                         std::uint16_t     screen_width,
                         std::uint16_t     screen_height,
                         std::uint8_t      root_depth,
                         std::uint32_t     root_visual,
                         std::uint8_t      image_byte_order ) noexcept;

            xcb_connection_t*          connection_       = nullptr;
            std::uint32_t              root_             = 0U;
            std::uint16_t              screen_width_     = 0U;
            std::uint16_t              screen_height_    = 0U;
            std::uint8_t               root_depth_       = 0U;
            std::uint32_t              root_visual_      = 0U;
            std::uint8_t               image_byte_order_ = 0U;
            std::vector<std::uint32_t> redirected_windows_;
    };

}    // namespace grab::screen

#endif    // GRAB_SCREEN_X11_CAPTURE_HPP
