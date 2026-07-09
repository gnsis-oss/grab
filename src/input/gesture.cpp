#include "grab/geometry/curve.hpp"
#include "grab/geometry/point.hpp"
#include "grab/window.hpp"
#include "input/gesture.hpp"
#include "input/input_sink.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace grab::input
{

    namespace
    {

        namespace geometry                                            = grab::geometry;

        constexpr bool             button_press                       = true;
        constexpr bool             button_release                     = false;
        constexpr bool             clear_modifiers                    = true;
        constexpr bool             keep_modifiers                     = false;
        constexpr std::int64_t     midpoint_divisor                   = 2L;
        constexpr std::int64_t     zero_long                          = 0L;
        constexpr double           zero_double                        = 0.0;
        constexpr double           cubic_control_divisor              = 3.0;
        constexpr std::uint32_t    menu_open_wait_millis              = 450U;
        constexpr std::uint32_t    menu_item_wait_millis              = 900U;
        constexpr std::uint32_t    drag_simple_press_wait_millis      = 200U;
        constexpr std::uint32_t    drag_simple_mid_wait_millis        = 100U;
        constexpr std::uint32_t    drag_simple_end_wait_millis        = 350U;
        constexpr std::uint32_t    drag_curve_initial_wait_millis     = 150U;
        constexpr std::uint32_t    drag_curve_press_wait_millis       = 200U;
        constexpr std::uint32_t    drag_curve_step_wait_millis        = 60U;
        constexpr std::uint32_t    drag_curve_arm_dwell_wait_millis   = 400U;
        constexpr std::uint32_t    drag_curve_cross_dwell_wait_millis = 300U;
        constexpr std::uint32_t    drag_curve_nudge_wait_millis       = 140U;
        constexpr std::uint32_t    drag_curve_release_wait_millis     = 500U;
        constexpr std::uint32_t    set_combo_click_wait_millis        = 200U;
        constexpr std::uint32_t    set_combo_return_wait_millis       = 400U;
        constexpr std::uint32_t    load_file_open_wait_millis         = 1'100U;
        constexpr std::uint32_t    load_file_type_wait_millis         = 250U;
        constexpr std::uint32_t    load_file_return_wait_millis       = 1'600U;
        constexpr std::uint32_t    open_stream_open_wait_millis       = 700U;
        constexpr std::uint32_t    open_stream_combo_wait_millis      = 300U;
        constexpr std::uint32_t    open_stream_connect_wait_millis    = 1'300U;
        constexpr std::string_view return_keysym                      = "Return";

        [[nodiscard]]
        std::int32_t
        to_coord( std::int64_t value ) noexcept
        {
            return static_cast<std::int32_t>( value );
        }

        [[nodiscard]]
        double
        drag_control_delta( std::int32_t source,
                            std::int32_t destination,
                            double       maximum ) noexcept
        {
            const auto delta = static_cast<std::int64_t>( destination ) -
                               static_cast<std::int64_t>( source );
            if( delta == zero_long )
            {
                return zero_double;
            }

            const auto span = static_cast<double>( delta > zero_long ? delta : -delta );
            const auto magnitude = std::min( maximum, span / cubic_control_divisor );
            if( delta > zero_long )
            {
                return magnitude;
            }
            return -magnitude;
        }

        [[nodiscard]]
        Point
        midpoint( Point lhs,
                  Point rhs ) noexcept
        {
            return Point{
                .x = to_coord(
                    geometry::floor_div( static_cast<std::int64_t>( lhs.x ) +
                                             static_cast<std::int64_t>( rhs.x ),
                                         midpoint_divisor )
                ),
                .y = to_coord(
                    geometry::floor_div( static_cast<std::int64_t>( lhs.y ) +
                                             static_cast<std::int64_t>( rhs.y ),
                                         midpoint_divisor )
                ),
            };
        }

        [[nodiscard]]
        geometry::PointF
        to_point_f( Point point ) noexcept
        {
            return geometry::PointF{
                .x = static_cast<double>( point.x ),
                .y = static_cast<double>( point.y ),
            };
        }

        [[nodiscard]]
        geometry::Curve
        drag_path( Point source,
                   Point destination )
        {
            const auto source_f      = to_point_f( source );
            const auto destination_f = to_point_f( destination );
            const auto control_x =
                drag_control_delta( source.x,
                                    destination.x,
                                    static_cast<double>( drag_final_arm_offset ) );
            const auto control_y =
                drag_control_delta( source.y,
                                    destination.y,
                                    static_cast<double>( drag_arm_y_lift ) );

            return geometry::Curve::cubic( source_f,
                                           source_f.translated( control_x, control_y ),
                                           destination_f.translated( -control_x,
                                                                     -control_y ),
                                           destination_f );
        }

        void
        click_at( InputSink&   sink,
                  Point        point,
                  std::uint8_t button )
        {
            sink.move( point );
            sink.button( button, button_press, keep_modifiers );
            sink.button( button, button_release, keep_modifiers );
        }

    }    // namespace

    void
    click_frac( InputSink&              sink,
                const grab::WindowRect& rect,
                double                  fx,
                double                  fy,
                std::uint8_t            button )
    {
        click_at( sink, rect.point_at_fraction( fx, fy ), button );
    }

    void
    click_off( InputSink&              sink,
               const grab::WindowRect& rect,
               std::int32_t            ox,
               std::int32_t            oy,
               std::uint8_t            button )
    {
        click_at( sink, rect.point_at_offset( ox, oy ), button );
    }

    void
    right_click_node( InputSink&              sink,
                      const grab::WindowRect& rect,
                      std::int32_t            ox,
                      std::int32_t            oy )
    {
        click_off( sink, rect, ox, oy, right_button );
    }

    void
    key( InputSink&       sink,
         std::string_view keysym )
    {
        sink.key( keysym );
    }

    void
    type_text( InputSink&       sink,
               std::string_view text )
    {
        sink.type_text( text );
    }

    void
    sleep_ms( InputSink&    sink,
              std::uint32_t millis )
    {
        sink.wait( millis );
    }

    void
    activate( InputSink& sink )
    {
        sink.activate();
    }

    void
    menu_open( InputSink&              sink,
               const grab::WindowRect& rect,
               double                  header_fx,
               double                  header_fy )
    {
        activate( sink );
        click_frac( sink, rect, header_fx, header_fy );
        sink.wait( menu_open_wait_millis );
    }

    void
    menu_item( InputSink&              sink,
               const grab::WindowRect& rect,
               double                  header_fx,
               double                  item_fx,
               std::int32_t            index,
               double                  header_fy )
    {
        menu_open( sink, rect, header_fx, header_fy );
        const auto item_fy =
            menu_first_item_fy + ( static_cast<double>( index ) * menu_item_spacing_fy );
        click_frac( sink, rect, item_fx, item_fy );
        sink.wait( menu_item_wait_millis );
    }

    void
    drag_simple( InputSink&              sink,
                 const grab::WindowRect& rect,
                 std::int32_t            from_ox,
                 std::int32_t            from_oy,
                 double                  to_fx,
                 double                  to_fy )
    {
        const auto first  = rect.point_at_offset( from_ox, from_oy );
        const auto last   = rect.point_at_fraction( to_fx, to_fy );
        const auto middle = midpoint( first, last );

        sink.move( first );
        sink.button( left_button, button_press, keep_modifiers );
        sink.wait( drag_simple_press_wait_millis );
        sink.move( middle );
        sink.sync();
        sink.wait( drag_simple_mid_wait_millis );
        sink.move( last );
        sink.sync();
        sink.wait( drag_simple_end_wait_millis );
        sink.button( left_button, button_release, keep_modifiers );
    }

    void
    drag_curve( InputSink&              sink,
                const grab::WindowRect& rect,
                double                  src_fx,
                double                  src_fy,
                double                  dst_fx,
                double                  dst_fy )
    {
        const auto source      = rect.point_at_fraction( src_fx, src_fy );
        const auto destination = rect.point_at_fraction( dst_fx, dst_fy );

        sink.move( source );
        sink.sync();
        sink.wait( drag_curve_initial_wait_millis );
        sink.button( left_button, button_press, clear_modifiers );
        sink.wait( drag_curve_press_wait_millis );

        for( const auto offset : drag_arm_offsets )
        {
            sink.move( source.translated( offset, drag_arm_y_lift ) );
            sink.sync();
            sink.wait( drag_curve_step_wait_millis );
        }

        sink.wait( drag_curve_arm_dwell_wait_millis );
        const auto path    = drag_path( source, destination );
        const auto samples = path.sample( static_cast<std::size_t>( drag_cross_steps ) );
        for( const auto point : samples )
        {
            sink.move( point );
            sink.sync();
            sink.wait( drag_curve_step_wait_millis );
        }

        sink.wait( drag_curve_cross_dwell_wait_millis );
        for( const auto nudge : drag_nudges )
        {
            sink.move( destination.translated( nudge.x, nudge.y ) );
            sink.sync();
            sink.wait( drag_curve_nudge_wait_millis );
        }

        sink.button( left_button, button_release, clear_modifiers );
        sink.wait( drag_curve_release_wait_millis );
    }

    void
    set_combo( InputSink&              sink,
               const grab::WindowRect& rect,
               double                  fx,
               double                  fy,
               std::string_view        value )
    {
        click_frac( sink, rect, fx, fy );
        sink.wait( set_combo_click_wait_millis );
        type_text( sink, value );
        key( sink, return_keysym );
        sink.wait( set_combo_return_wait_millis );
    }

    void
    load_file( InputSink&              sink,
               const grab::WindowRect& rect,
               std::string_view        path )
    {
        activate( sink );
        click_frac( sink, rect, load_file_button_fx, load_file_button_fy );
        sink.wait( load_file_open_wait_millis );
        type_text( sink, path );
        sink.wait( load_file_type_wait_millis );
        key( sink, return_keysym );
        sink.wait( load_file_return_wait_millis );
    }

    void
    open_stream( InputSink&              sink,
                 const grab::WindowRect& rect,
                 std::string_view        source )
    {
        activate( sink );
        click_frac( sink, rect, open_stream_button_fx, open_stream_button_fy );
        sink.wait( open_stream_open_wait_millis );
        set_combo( sink, rect, open_stream_source_fx, open_stream_source_fy, source );
        sink.wait( open_stream_combo_wait_millis );
        click_frac( sink, rect, open_stream_connect_fx, open_stream_connect_fy );
        sink.wait( open_stream_connect_wait_millis );
    }

}    // namespace grab::input
