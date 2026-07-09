#ifndef GRAB_SCREEN_VIRTUAL_DISPLAY_HPP
#define GRAB_SCREEN_VIRTUAL_DISPLAY_HPP

#include "grab/result.hpp"

#include <cstdint>
#include <string>
#include <sys/types.h>

namespace grab::screen
{

    class VirtualDisplay
    {
        public:

            [[nodiscard]]
            static grab::Result<VirtualDisplay>
            start( std::uint16_t width,
                   std::uint16_t height,
                   std::uint8_t  depth = 24U );

            ~VirtualDisplay();

            VirtualDisplay( const VirtualDisplay& ) = delete;
            VirtualDisplay&
            operator=( const VirtualDisplay& ) = delete;
            VirtualDisplay( VirtualDisplay&& other ) noexcept;
            VirtualDisplay&
            operator=( VirtualDisplay&& other ) noexcept;

            [[nodiscard]]
            const std::string&
            display() const noexcept;

        private:

            VirtualDisplay( pid_t       child_pid,
                            std::string display ) noexcept;

            auto
                        stop() noexcept -> void;

            pid_t       child_pid_ = -1;
            std::string display_;
    };

}    // namespace grab::screen

#endif    // GRAB_SCREEN_VIRTUAL_DISPLAY_HPP
