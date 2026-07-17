#pragma once

#include "grab/result.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

namespace grab::core
{

    class Reactor
    {
        public:

            Reactor();
            ~Reactor();

            Reactor( const Reactor& ) = delete;
            Reactor&
            operator=( const Reactor& ) = delete;
            Reactor( Reactor&& )        = delete;
            Reactor&
            operator=( Reactor&& ) = delete;

            [[nodiscard]]
            grab::Result<void>
            run();

            void
            stop() noexcept;

            std::uint64_t
            add_fd( int                                  fd,
                    std::uint32_t                        events,
                    std::function<void( std::uint32_t )> cb );

            void
            remove_fd( std::uint64_t token );

            std::uint64_t
            add_timer( std::chrono::nanoseconds delay,
                       std::function<void()>    cb );

            void
            post( std::function<void()> fn );

        private:

            class Impl;

            std::unique_ptr<Impl> impl_;
    };

}    // namespace grab::core
