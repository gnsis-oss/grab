#pragma once

#include "grab/geometry/point.hpp"
#include "grab/result.hpp"

#include <cstdint>
#include <string_view>

namespace grab::platform::x11
{

    class XcbConnection;

    // An XInput2 master pointer/keyboard pair (a second seat) on an existing X
    // server. Creating a seat adds a master device pair via XIChangeHierarchy;
    // destroying it removes them again. The connection must outlive the seat.
    class XiSeat
    {
        public:

            using PointerPos = grab::geometry::PointF;

            [[nodiscard]]
            static grab::Result<XiSeat>
            create( const XcbConnection& conn,
                    std::string_view     name );

            XiSeat()                = delete;
            XiSeat( const XiSeat& ) = delete;
            XiSeat&
            operator=( const XiSeat& ) = delete;

            XiSeat( XiSeat&& other ) noexcept;
            XiSeat&
            operator=( XiSeat&& other ) noexcept;

            ~XiSeat();

            [[nodiscard]]
            std::uint16_t
            pointer_id() const noexcept;

            [[nodiscard]]
            std::uint16_t
            keyboard_id() const noexcept;

            // A master pointer that is NOT this seat (the human's virtual core
            // pointer), used to prove seat isolation.
            [[nodiscard]]
            std::uint16_t
            primary_pointer_id() const noexcept;

            // Move this seat's pointer to absolute root coordinates.
            [[nodiscard]]
            grab::Result<void>
            warp_to( std::int16_t x,
                     std::int16_t y );

            // Root-relative position of the given master pointer device.
            [[nodiscard]]
            grab::Result<PointerPos>
            query( std::uint16_t device_id ) const;

        private:

            XiSeat( const XcbConnection& conn,
                    std::uint16_t        pointer_id,
                    std::uint16_t        keyboard_id,
                    std::uint16_t        primary_pointer_id ) noexcept;

            void
                                 remove() noexcept;

            const XcbConnection* conn                      = nullptr;
            std::uint16_t        pointer_device_id         = 0;
            std::uint16_t        keyboard_device_id        = 0;
            std::uint16_t        primary_pointer_device_id = 0;
            bool                 active                    = false;
    };

}    // namespace grab::platform::x11
