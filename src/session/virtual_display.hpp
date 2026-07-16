#pragma once

#include "grab/process_ref.hpp"
#include "grab/result.hpp"

#include <cstdint>
#include <optional>
#include <string>

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

            VirtualDisplay( grab::OwnedProcess child,
                            std::string        display ) noexcept;

            auto
                                              stop() noexcept -> void;

            std::optional<grab::OwnedProcess> child_;
            std::string                       display_;
    };

}    // namespace grab::screen
