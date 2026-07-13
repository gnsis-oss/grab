#pragma once

#include "grab/drag.hpp"
#include "grab/keymap.hpp"
#include "grab/pointer_button.hpp"
#include "grab/result.hpp"
#include "input/gestures.hpp"
#include "input/locator.hpp"
#include "input/seat.hpp"

#include <cstdint>
#include <optional>
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
                  std::string_view layout  = {} );

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
            click( std::uint8_t button = grab::input::primaryButton );

            [[nodiscard]]
            grab::Result<void>
            click_at( std::int16_t x,
                      std::int16_t y,
                      std::uint8_t button = grab::input::primaryButton );

            [[nodiscard]]
            grab::Result<void>
            drag( grab::input::Point              from,
                  grab::input::Point              to,
                  const grab::input::DragOptions& options = {} );

            [[nodiscard]]
            grab::Result<void>
            type_text( std::string_view utf8 );

            [[nodiscard]]
            grab::Result<void>
            press_key( std::string_view name );

            [[nodiscard]]
            grab::Result<void>
            activate( const grab::input::LocatedWindow& win );

            [[nodiscard]]
            grab::Result<grab::input::LocatedWindow>
            locate( const std::vector<std::string>& wm_class_candidates,
                    std::string_view                title = {} );

            [[nodiscard]]
            grab::Result<void>
            click_in_window( const grab::input::LocatedWindow& win,
                             double                            frac_x,
                             double                            frac_y,
                             std::uint8_t button = grab::input::primaryButton );

            [[nodiscard]]
            grab::Result<void>
            drag_curve_in_window( const grab::input::LocatedWindow& win,
                                  double                            source_x,
                                  double                            source_y,
                                  double                            destination_x,
                                  double                            destination_y,
                                  const grab::input::DragOptions&   options = {} );

        private:

            Input( grab::input::Seat           seat,
                   std::optional<grab::Keymap> keymap,
                   grab::input::WindowLocator  locator,
                   std::string                 display,
                   bool                        server_keymap ) noexcept;

            [[nodiscard]]
            grab::Result<grab::Keymap*>
                                        ensure_keymap();

            grab::input::Seat           seat_;
            std::optional<grab::Keymap> keymap_;
            grab::input::WindowLocator  locator_;
            std::string                 display_;
            bool                        server_keymap_ = false;
    };

}    // namespace grab
