#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/result.hpp"

#include <string>

namespace grab::spi
{

    struct EventSpec
    {
            std::string name;
            friend bool
            operator==( const EventSpec&,
                        const EventSpec& ) = default;
    };

    class EventSource
    {
        public:

            EventSource()                     = default;
            virtual ~EventSource()            = default;
            EventSource( const EventSource& ) = delete;
            EventSource&
            operator=( const EventSource& ) = delete;
            EventSource( EventSource&& )    = delete;
            EventSource&
            operator=( EventSource&& ) = delete;

            [[nodiscard]]
            virtual Result<void>
            enable( const EventSpec& spec ) = 0;

            [[nodiscard]]
            virtual Result<void>
            disable( const EventSpec& spec ) = 0;
    };

}    // namespace grab::spi
