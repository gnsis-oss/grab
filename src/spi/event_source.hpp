#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/context.hpp"
#include "grab/event.hpp"
#include "grab/result.hpp"

#include <chrono>
#include <functional>
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

            using EventSink        = std::function<void( grab::Event&& )>;

            EventSource()          = default;
            virtual ~EventSource() = default;

            // Producer sources (X11, fake) override this to deliver each decoded
            // grab::Event; sources that self-publish (e.g. AT-SPI) ignore it.
            virtual void
            set_sink( EventSink )
            {
            }

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

            // Blocks until a relevant event arrives or maximum_wait elapses.
            // Implementations must also honor the context deadline/cancellation.
            [[nodiscard]]
            virtual Result<void>
            wait_for_event( const EventSpec&         spec,
                            const OperationContext&  context,
                            std::chrono::nanoseconds maximum_wait ) = 0;
    };

}    // namespace grab::spi
