#pragma once

#include "grab/geometry/point.hpp"

#include <cstdint>
#include <string_view>

namespace grab::input
{

    using Point = grab::geometry::Point;

    class InputSink
    {
        public:

            InputSink()                       = default;
            InputSink( const InputSink& )     = delete;
            InputSink( InputSink&& ) noexcept = delete;
            InputSink&
            operator=( const InputSink& ) = delete;
            InputSink&
            operator=( InputSink&& ) noexcept = delete;
            virtual ~InputSink()              = default;

            virtual void
            move( Point p ) = 0;
            virtual void
            button( std::uint8_t code,
                    bool         press,
                    bool         clear_modifiers ) = 0;
            virtual void
            sync() = 0;
            virtual void
            wait( std::uint32_t millis ) = 0;
            virtual void
            type_text( std::string_view utf8 ) = 0;
            virtual void
            key( std::string_view keysym ) = 0;
            virtual void
            activate() = 0;
    };

}    // namespace grab::input
