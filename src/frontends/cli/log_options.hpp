#pragma once

// Global logging flags, accepted by every verb.
//
//   --log-level off|nominal|verbose|debug
//   --log-tags  csv
//   --log-file  path
//
// They are stripped before verb dispatch, because each verb parses its own
// flags strictly and rejects anything it does not recognise. Environment
// variables (GRAB_LOG, GRAB_LOG_TAGS, GRAB_LOG_FILE) are consulted first, so
// an explicit flag always wins over the environment.

#include "grab/result.hpp"

#include <span>
#include <string_view>
#include <vector>

namespace grab::cli
{

    struct LogOptions
    {
            std::vector<char*> remaining;
    };

    // Applies any logging flags found in `args` and returns the arguments with
    // them removed. Fails on an unknown level name or a file that cannot be
    // opened — silently logging nowhere after being asked for a file is worse
    // than refusing to start.
    [[nodiscard]]
    Result<LogOptions>
    apply_log_options( std::span<char* const> args );

    [[nodiscard]]
    std::string_view
    log_options_usage() noexcept;

}    // namespace grab::cli
