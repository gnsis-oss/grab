#pragma once

#include "core/environment.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace grab::core
{

    struct SystemFacts
    {
            std::function<std::optional<std::string>( std::string_view )> get_env;
            std::function<bool( const std::string& )>                     path_readable;
            std::function<bool( const std::string& )>                     path_writable;
            std::function<std::vector<std::string>()> list_input_devices;
    };

    [[nodiscard]]
    Environment
    probe_environment( const SystemFacts& facts );

    [[nodiscard]]
    SystemFacts
    real_system_facts();

}    // namespace grab::core
