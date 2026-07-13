#pragma once

#include "grab/drag.hpp"
#include "grab/geometry.hpp"
#include "grab/pointer_button.hpp"
#include "grab/result.hpp"
#include "grab/window_match.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace grab::input
{

    using grab::geometry::Point;

}    // namespace grab::input

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

            class Impl;

            explicit Input( std::unique_ptr<Impl> impl ) noexcept;

            [[nodiscard]]
            grab::Result<Impl*>
                                  require_impl() noexcept;

            std::unique_ptr<Impl> impl_;
    };

}    // namespace grab
