#pragma once

#include "grab/geometry/point.hpp"
#include "grab/result.hpp"

#include <cstdint>

struct xcb_connection_t;

namespace grab::input
{

    class Seat
    {
        public:

            [[nodiscard]]
            static grab::Result<Seat>
            open( const char* display = nullptr );

            ~Seat();

            Seat( const Seat& ) = delete;
            Seat&
            operator=( const Seat& ) = delete;
            Seat( Seat&& other ) noexcept;
            Seat&
            operator=( Seat&& other ) noexcept;

            [[nodiscard]]
            grab::Result<void>
            move_pointer_absolute( std::int16_t x,
                                   std::int16_t y );

            [[nodiscard]]
            grab::Result<void>
            button( std::uint8_t button,
                    bool         press );

            [[nodiscard]]
            grab::Result<void>
            key( std::uint8_t keycode,
                 bool         press );

            // The pointer's current position in root (screen) coordinates.
            // Queried from the X server, never inferred from previously
            // injected motion: a caller that has not moved the pointer, or
            // whose synthetic motion was overridden by the physical mouse,
            // otherwise has no way to learn where the cursor actually is.
            [[nodiscard]]
            grab::Result<grab::geometry::Point>
            pointer_position();

            [[nodiscard]]
            grab::Result<void>
            flush();

        private:

            Seat( xcb_connection_t* connection,
                  std::uint32_t     root ) noexcept;

            xcb_connection_t* connection_ = nullptr;
            std::uint32_t     root_       = 0;
    };

}    // namespace grab::input
