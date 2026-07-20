#pragma once

#include "config/batch_manifest.hpp"
#include "grab/config.hpp"
#include "grab/result.hpp"
#include "notify/notifier.hpp"

#include <cstdint>
#include <filesystem>

namespace grab::screen
{

    struct ConfigBatchResult
    {
            grab::config::BatchManifest manifest;
            std::filesystem::path       session_dir;
            std::uint32_t               target_errors{};
            std::uint32_t               compare_failures{};
    };

    [[nodiscard]]
    grab::Result<ConfigBatchResult>
    run_config_batch( const grab::config::Config& cfg,
                      grab::notify::Notifier*     notifier );

}    // namespace grab::screen
