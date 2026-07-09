#ifndef CORE_ENVIRONMENT_HPP
#define CORE_ENVIRONMENT_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace grab::core
{

    enum class SessionType : std::uint8_t
    {
        unknown,
        x11,
        wayland,
        headless,
        count,
    };

    struct InputDeviceAccess
    {
            std::string path;
            bool        readable = false;
    };

    struct Environment
    {
            SessionType                    session          = SessionType::unknown;
            bool                           xwayland_present = false;
            std::string                    compositor;
            std::string                    desktop;
            std::vector<InputDeviceAccess> input_devices;
            bool                           uinput_writable = false;
            std::uint64_t                  generation      = 0;
    };

}    // namespace grab::core

#endif    // CORE_ENVIRONMENT_HPP
