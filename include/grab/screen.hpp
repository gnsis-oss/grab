#pragma once

#include "grab/image.hpp"
#include "grab/result.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace grab
{

    class Screen
    {
        public:

            [[nodiscard]]
            static grab::Result<Screen>
            open( const char* display = nullptr );

            ~Screen();

            Screen( const Screen& ) = delete;
            Screen&
            operator=( const Screen& ) = delete;
            Screen( Screen&& other ) noexcept;
            Screen&
            operator=( Screen&& other ) noexcept;

            [[nodiscard]]
            grab::Result<Image>
            window( std::uint32_t id );

            [[nodiscard]]
            grab::Result<Image>
            window_by_class( const std::vector<std::string>& wm_class_candidates );

            [[nodiscard]]
            grab::Result<Image>
            display();

            [[nodiscard]]
            grab::Result<Image>
            region( std::int16_t  x,
                    std::int16_t  y,
                    std::uint16_t width,
                    std::uint16_t height );

            [[nodiscard]]
            grab::Result<Image>
            active_window();

        private:

            struct Impl;

            explicit Screen( std::unique_ptr<Impl> impl ) noexcept;

            std::unique_ptr<Impl> impl_;
    };

}    // namespace grab
