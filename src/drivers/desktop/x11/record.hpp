#pragma once

#include "grab/result.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace grab::core
{

    class Reactor;

}

namespace grab::screen
{

    struct RecordOptions
    {
            std::string   display;
            std::string   path;
            std::uint32_t fps        = 10U;
            std::uint32_t max_frames = 0U;
    };

    class Recorder
    {
        public:

            [[nodiscard]]
            static grab::Result<Recorder>
            start( grab::core::Reactor& reactor,
                   const RecordOptions& options );

            ~Recorder();

            Recorder( const Recorder& ) = delete;
            Recorder&
            operator=( const Recorder& ) = delete;
            Recorder( Recorder&& other ) noexcept;
            Recorder&
            operator=( Recorder&& other ) noexcept;

            [[nodiscard]]
            grab::Result<void>
            stop();

        private:

            struct State;

            explicit Recorder( std::shared_ptr<State> state ) noexcept;

            std::shared_ptr<State> state_;
    };

}    // namespace grab::screen
