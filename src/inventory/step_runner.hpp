#pragma once

#include "grab/window.hpp"
#include "input/input_sink.hpp"
#include "inventory/action.hpp"

namespace grab::inventory
{

    void
    run_step( grab::input::InputSink& sink,
              const grab::WindowRect& rect,
              const Step&             step );

}    // namespace grab::inventory
