#pragma once

#include "grab/window.hpp"
#include "input/input_sink.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace grab::input
{

    inline constexpr std::uint8_t left_button           = 1U;
    inline constexpr std::uint8_t right_button          = 3U;

    inline constexpr double       menu_first_item_fy    = 0.0475;
    inline constexpr double       menu_item_spacing_fy  = 0.0266;
    inline constexpr double       menu_header_fy        = 0.0153;

    inline constexpr std::size_t  drag_arm_offset_count = 6U;
    inline constexpr std::array<std::int32_t, drag_arm_offset_count> drag_arm_offsets{
        10,
        20,
        32,
        46,
        62,
        80,
    };
    inline constexpr std::int32_t                        drag_final_arm_offset = 80;
    inline constexpr std::int32_t                        drag_cross_steps      = 16;
    inline constexpr std::int32_t                        drag_arm_y_lift       = 6;

    inline constexpr std::size_t                         drag_nudge_count      = 4U;
    inline constexpr std::array<Point, drag_nudge_count> drag_nudges{
        Point{ .x = 0,  .y = 0},
        Point{ .x = 4,  .y = 3},
        Point{.x = -3,  .y = 2},
        Point{ .x = 2, .y = -2},
    };

    inline constexpr double load_file_button_fx    = 0.010;
    inline constexpr double load_file_button_fy    = 0.084;

    inline constexpr double open_stream_button_fx  = 0.130;
    inline constexpr double open_stream_button_fy  = 0.050;
    inline constexpr double open_stream_source_fx  = 0.078;
    inline constexpr double open_stream_source_fy  = 0.085;
    inline constexpr double open_stream_connect_fx = 0.168;
    inline constexpr double open_stream_connect_fy = 0.085;

    void
    click_frac( InputSink&              sink,
                const grab::WindowRect& rect,
                double                  fx,
                double                  fy,
                std::uint8_t            button = left_button );

    void
    click_off( InputSink&              sink,
               const grab::WindowRect& rect,
               std::int32_t            ox,
               std::int32_t            oy,
               std::uint8_t            button = left_button );

    void
    right_click_node( InputSink&              sink,
                      const grab::WindowRect& rect,
                      std::int32_t            ox,
                      std::int32_t            oy );

    void
    key( InputSink&       sink,
         std::string_view keysym );

    void
    type_text( InputSink&       sink,
               std::string_view text );

    void
    sleep_ms( InputSink&    sink,
              std::uint32_t millis );

    void
    activate( InputSink& sink );

    void
    menu_open( InputSink&              sink,
               const grab::WindowRect& rect,
               double                  header_fx,
               double                  header_fy = menu_header_fy );

    void
    menu_item( InputSink&              sink,
               const grab::WindowRect& rect,
               double                  header_fx,
               double                  item_fx,
               std::int32_t            index,
               double                  header_fy = menu_header_fy );

    void
    drag_simple( InputSink&              sink,
                 const grab::WindowRect& rect,
                 std::int32_t            from_ox,
                 std::int32_t            from_oy,
                 double                  to_fx,
                 double                  to_fy );

    void
    drag_curve( InputSink&              sink,
                const grab::WindowRect& rect,
                double                  src_fx,
                double                  src_fy,
                double                  dst_fx,
                double                  dst_fy );

    void
    set_combo( InputSink&              sink,
               const grab::WindowRect& rect,
               double                  fx,
               double                  fy,
               std::string_view        value );

    void
    load_file( InputSink&              sink,
               const grab::WindowRect& rect,
               std::string_view        path );

    void
    open_stream( InputSink&              sink,
                 const grab::WindowRect& rect,
                 std::string_view        source );

}    // namespace grab::input
