#pragma once

#include "grab/result.hpp"
#include "grab/workspace.hpp"

#include <span>
#include <string_view>

namespace grab::cli
{

    [[nodiscard]]
    grab::Result<grab::WorkspaceDescriptor>
    parse_session_start_args( std::span<const std::string_view> args );

    [[nodiscard]]
    bool
    is_session_subcommand( std::span<const std::string_view> args ) noexcept;

    int
    run_session_command( std::span<const std::string_view> args );

}    // namespace grab::cli
