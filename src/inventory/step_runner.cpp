#include "grab/window.hpp"
#include "input/gesture.hpp"
#include "input/input_sink.hpp"
#include "inventory/action.hpp"
#include "inventory/step_runner.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <variant>

namespace grab::inventory
{

    namespace
    {

        constexpr double millis_per_second = 1'000.0;
        constexpr auto   minimum_millis    = std::int64_t{ 0 };
        constexpr auto   maximum_millis =
            static_cast<std::int64_t>( std::numeric_limits<std::uint32_t>::max() );

        [[nodiscard]]
        std::uint32_t
        sleep_millis( double seconds )
        {
            const auto rounded = static_cast<std::int64_t>(
                std::llround( std::max( seconds * millis_per_second, 0.0 ) )
            );
            const auto clamped = std::clamp( rounded, minimum_millis, maximum_millis );
            return static_cast<std::uint32_t>( clamped );
        }

        void
        run_one_step( grab::input::InputSink& sink,
                      const grab::WindowRect& rect,
                      const ActivateStep&     step )
        {
            ( void )rect;
            ( void )step;
            grab::input::activate( sink );
        }

        void
        run_one_step( grab::input::InputSink& sink,
                      const grab::WindowRect& rect,
                      const KeyStep&          step )
        {
            ( void )rect;
            grab::input::key( sink, step.keys );
        }

        void
        run_one_step( grab::input::InputSink& sink,
                      const grab::WindowRect& rect,
                      const TypeStep&         step )
        {
            ( void )rect;
            grab::input::type_text( sink, step.text );
        }

        void
        run_one_step( grab::input::InputSink& sink,
                      const grab::WindowRect& rect,
                      const SleepStep&        step )
        {
            ( void )rect;
            grab::input::sleep_ms( sink, sleep_millis( step.seconds ) );
        }

        void
        run_one_step( grab::input::InputSink& sink,
                      const grab::WindowRect& rect,
                      const ClickFracStep&    step )
        {
            grab::input::click_frac( sink, rect, step.fx, step.fy, step.button );
        }

        void
        run_one_step( grab::input::InputSink& sink,
                      const grab::WindowRect& rect,
                      const ClickOffStep&     step )
        {
            grab::input::click_off( sink, rect, step.ox, step.oy, step.button );
        }

        void
        run_one_step( grab::input::InputSink& sink,
                      const grab::WindowRect& rect,
                      const MenuOpenStep&     step )
        {
            grab::input::menu_open( sink, rect, step.header_fx, step.header_fy );
        }

        void
        run_one_step( grab::input::InputSink& sink,
                      const grab::WindowRect& rect,
                      const MenuItemStep&     step )
        {
            grab::input::menu_item( sink,
                                    rect,
                                    step.header_fx,
                                    step.item_fx,
                                    step.index,
                                    step.header_fy );
        }

        void
        run_one_step( grab::input::InputSink&   sink,
                      const grab::WindowRect&   rect,
                      const RightClickNodeStep& step )
        {
            grab::input::right_click_node( sink, rect, step.ox, step.oy );
        }

        void
        run_one_step( grab::input::InputSink& sink,
                      const grab::WindowRect& rect,
                      const DragCurveStep&    step )
        {
            grab::input::drag_curve( sink,
                                     rect,
                                     step.src_fx,
                                     step.src_fy,
                                     step.dst_fx,
                                     step.dst_fy );
        }

        void
        run_one_step( grab::input::InputSink& sink,
                      const grab::WindowRect& rect,
                      const DragFracStep&     step )
        {
            grab::input::drag_simple( sink,
                                      rect,
                                      step.from_ox,
                                      step.from_oy,
                                      step.to_fx,
                                      step.to_fy );
        }

        void
        run_one_step( grab::input::InputSink& sink,
                      const grab::WindowRect& rect,
                      const LoadFileStep&     step )
        {
            grab::input::load_file( sink, rect, step.path );
        }

        void
        run_one_step( grab::input::InputSink& sink,
                      const grab::WindowRect& rect,
                      const OpenStreamStep&   step )
        {
            grab::input::open_stream( sink, rect, step.source );
        }

        void
        run_one_step( grab::input::InputSink& sink,
                      const grab::WindowRect& rect,
                      const SetComboStep&     step )
        {
            grab::input::set_combo( sink, rect, step.fx, step.fy, step.value );
        }

    }    // namespace

    void
    run_step( grab::input::InputSink& sink,
              const grab::WindowRect& rect,
              const Step&             step )
    {
        std::visit(
            [&sink, &rect]( const auto& item )
            {
                run_one_step( sink, rect, item );
            },
            step
        );
    }

}    // namespace grab::inventory
