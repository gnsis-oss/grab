#pragma once

#include "grab/result.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace grab::config
{

    enum class RunState : std::uint8_t
    {
        Running,
        Done,
        Failed,
        Count,
    };

    struct TargetOutcome
    {
            std::string              name;
            std::vector<std::string> argv;
            std::int64_t             pid{ -1 };
            std::uint32_t            window_id{};
            std::vector<std::string> files;
            std::string              error;
    };

    struct FileCompareEntry
    {
            std::string name;
            double      score{};
            bool        passed{};
    };

    struct BatchManifest
    {
            std::filesystem::path         profile;
            std::string                   started_at;
            std::string                   ended_at;
            RunState                      state{ RunState::Running };
            std::vector<TargetOutcome>    targets;
            std::vector<FileCompareEntry> compare;

            [[nodiscard]]
            grab::Result<void>
            write( const std::filesystem::path& session_dir ) const;

            [[nodiscard]]
            static grab::Result<BatchManifest>
            read( const std::filesystem::path& session_dir );
    };

}    // namespace grab::config
