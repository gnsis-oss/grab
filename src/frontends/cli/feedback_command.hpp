#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/result.hpp"
#include "grab/session.hpp"

#include <span>
#include <string_view>

namespace grab::cli
{

    [[nodiscard]]
    Result<CursorFeedbackConfig>
    parse_feedback_options( std::span<const std::string_view> args );

    int
    run_feedback_command( std::span<char* const> args );

}    // namespace grab::cli
