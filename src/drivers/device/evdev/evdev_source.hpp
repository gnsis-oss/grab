#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/event.hpp"
#include "grab/result.hpp"
#include "spi/event_source.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace grab::drivers::device::evdev
{

    // Pull-based evdev event source. Opens a Linux /dev/input character device (or
    // adopts an already-open fd), and on each wait_for_event() drains complete
    // input_event records from the fd, decodes KeyDown/KeyUp/MouseMove, and delivers
    // them through the sink installed by set_sink(). enable()/disable() track per-spec
    // demand; the source itself always reads every record — downstream demand filtering
    // is the bus's job.
    class EvdevSource final : public grab::spi::EventSource
    {
        public:

            using EventSink = grab::spi::EventSource::EventSink;

            [[nodiscard]]
            static grab::Result<std::unique_ptr<EvdevSource>>
            open_device( const std::string& path );

            [[nodiscard]]
            static grab::Result<std::unique_ptr<EvdevSource>>
            adopt_fd( int fd );

            ~EvdevSource() override;

            EvdevSource( const EvdevSource& ) = delete;
            EvdevSource&
            operator=( const EvdevSource& ) = delete;
            EvdevSource( EvdevSource&& )    = delete;
            EvdevSource&
            operator=( EvdevSource&& ) = delete;

            void
            set_sink( EventSink sink ) override;

            [[nodiscard]]
            grab::Result<void>
            enable( const grab::spi::EventSpec& spec ) override;

            [[nodiscard]]
            grab::Result<void>
            disable( const grab::spi::EventSpec& spec ) override;

            [[nodiscard]]
            grab::Result<void>
            wait_for_event( const grab::spi::EventSpec&   spec,
                            const grab::OperationContext& context,
                            std::chrono::nanoseconds      maximum_wait ) override;

        private:

            explicit EvdevSource( int fd ) noexcept;

            int                   fd_ = -1;
            std::vector<char>     buffer_;
            std::mutex            sink_mutex_;
            EventSink             sink_;
            std::mutex            state_mutex_;
            std::set<std::string> enabled_;
            bool                  active_ = true;
    };

}    // namespace grab::drivers::device::evdev
