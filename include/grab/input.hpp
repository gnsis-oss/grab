#pragma once

#include "grab/result.hpp"
#include "input/gestures.hpp"
#include "input/keymap.hpp"
#include "input/locator.hpp"
#include "input/seat.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace grab
{

    class Input
    {
        public:

            [[nodiscard]]
            static grab::Result<Input>
            open( const char*      display = nullptr,
                  std::string_view layout  = "us" );

            ~Input();

            Input( const Input& ) = delete;
            Input&
            operator=( const Input& ) = delete;
            Input( Input&& other ) noexcept;
            Input&
            operator=( Input&& other ) noexcept;

            [[nodiscard]]
            grab::Result<void>
            move( std::int16_t x,
                  std::int16_t y );

            [[nodiscard]]
            grab::Result<void>
            click( std::uint8_t button = 1U );

            [[nodiscard]]
            grab::Result<void>
            click_at( std::int16_t x,
                      std::int16_t y,
                      std::uint8_t button = 1U );

            [[nodiscard]]
            grab::Result<void>
            drag( grab::input::Point from,
                  grab::input::Point to );

            [[nodiscard]]
            grab::Result<void>
            type_text( std::string_view utf8 );

            [[nodiscard]]
            grab::Result<grab::input::LocatedWindow>
            locate( const std::vector<std::string>& wm_class_candidates,
                    std::string_view                title = {} );

            [[nodiscard]]
            grab::Result<void>
            click_in_window( const grab::input::LocatedWindow& win,
                             double                            frac_x,
                             double                            frac_y,
                             std::uint8_t                      button = 1U );

        private:

            Input( grab::input::Seat          seat,
                   grab::input::Keymap        keymap,
                   grab::input::WindowLocator locator ) noexcept;

            grab::input::Seat          seat_;
            grab::input::Keymap        keymap_;
            grab::input::WindowLocator locator_;
    };

}    // namespace grab
