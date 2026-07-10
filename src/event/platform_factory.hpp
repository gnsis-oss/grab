#pragma once

#include "event/source.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace grab::event
{

    struct SourceConfig
    {
            const char*                          display = nullptr;
            std::optional<std::filesystem::path> evdev_device;
            std::optional<int>                   browser_input_fd;
            bool                                 enable_input   = true;
            bool                                 enable_window  = true;
            bool                                 enable_a11y    = true;
            bool                                 enable_browser = false;
            bool                                 enable_state   = true;
            std::chrono::milliseconds            window_poll{ 100 };
            std::chrono::milliseconds            state_interval{ 60'000 };
    };

    class PlatformFactory
    {
        public:

            [[nodiscard]]
            static std::vector<std::unique_ptr<EventSource>>
            build( const SourceConfig& config );
    };

}    // namespace grab::event
