#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace grab::core
{

    enum class SessionType : std::uint8_t
    {
        Unknown,
        X11,
        Wayland,
        Headless,
        Count,
    };

    struct InputDeviceAccess
    {
            std::string path;
            bool        readable = false;
    };

    struct Environment
    {
            SessionType                    session          = SessionType::Unknown;
            bool                           xwayland_present = false;
            std::string                    compositor;
            std::string                    desktop;
            std::vector<InputDeviceAccess> input_devices;
            bool                           uinput_writable = false;
            std::uint64_t                  generation      = 0;
    };

}    // namespace grab::core
