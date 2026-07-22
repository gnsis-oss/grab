#pragma once

#include "drivers/desktop/x11/config_watch.hpp"
#include "grab/result.hpp"

#include <filesystem>
#include <span>
#include <string>
#include <utility>

namespace grab::cli
{

    struct DaemonPaths
    {
            std::filesystem::path pid_file;
            std::filesystem::path status_file;
            std::filesystem::path log_file;

            [[nodiscard]]
            static DaemonPaths
            standard();
    };

    [[nodiscard]]
    grab::Result<void>
    daemonize( const DaemonPaths& paths );

    [[nodiscard]]
    grab::Result<void>
    write_status( const DaemonPaths&                                   paths,
                  std::span<const std::pair<std::string,
                                            grab::screen::WatchStats>> stats );

    [[nodiscard]]
    int
    run_watch_stop( const DaemonPaths& paths );

    [[nodiscard]]
    int
    run_watch_status( const DaemonPaths& paths,
                      bool               as_json );

}    // namespace grab::cli
