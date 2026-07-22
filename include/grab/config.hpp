#pragma once

#include "grab/result.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace grab::config
{

    enum class DisplayBackend : std::uint8_t
    {
        Native,
        Xvfb,
        Count,
    };

    enum class MatchKind : std::uint8_t
    {
        Pid,
        WmClass,
        Title,
        WindowId,
        Count,
    };

    enum class StepAction : std::uint8_t
    {
        Move,
        Click,
        ClickAt,
        Drag,
        Type,
        Key,
        Delay,
        Count,
    };

    enum class NotifyStrategy : std::uint8_t
    {
        Os,
        None,
        Count,
    };

    enum class CompareMode : std::uint8_t
    {
        Exact,
        Rmse,
        Count,
    };

    inline constexpr double        defaultTimeoutSeconds   = 15.0;
    inline constexpr std::uint16_t defaultDisplayWidth     = 1'920U;
    inline constexpr std::uint16_t defaultDisplayHeight    = 1'080U;
    inline constexpr std::uint8_t  defaultDisplayDepth     = 24U;
    inline constexpr std::uint32_t defaultTargetIntervalMs = 200U;
    inline constexpr std::uint32_t defaultTargetSettleMs   = 1'000U;
    inline constexpr std::uint32_t defaultPopupTimeoutMs   = 2'000U;
    inline constexpr double        defaultCompareThreshold = 5.0;

    struct DefaultsSection
    {
            std::string           format{ "png" };
            double                timeout_s{ defaultTimeoutSeconds };
            bool                  kill_after{ true };
            std::filesystem::path output_root;
    };

    struct DisplaySection
    {
            DisplayBackend backend{ DisplayBackend::Native };
            std::uint16_t  width{ defaultDisplayWidth };
            std::uint16_t  height{ defaultDisplayHeight };
            std::uint8_t   depth{ defaultDisplayDepth };
    };

    struct TargetMatch
    {
            MatchKind     kind{ MatchKind::WmClass };
            std::string   text;
            std::uint32_t pid{};
            std::uint32_t window_id{};
    };

    struct WatchLimits
    {
            std::uint64_t max_files{};
            std::uint32_t max_age_days{};
            std::uint64_t max_disk_mib{};
    };

    struct WatchSection
    {
            std::uint32_t              interval_ms{};
            std::filesystem::path      output;
            std::string                filename{ "capture_{timestamp}" };
            std::string                format{ "png" };
            std::optional<TargetMatch> target;
            WatchLimits                limits;
    };

    struct ScriptStep
    {
            StepAction    action{ StepAction::Move };
            std::int16_t  x{};
            std::int16_t  y{};
            std::int16_t  to_x{};
            std::int16_t  to_y{};
            std::uint8_t  button{ 1U };
            std::string   text;
            std::uint32_t delay_ms{};
    };

    struct ScriptSection
    {
            bool                    loop{ false };
            std::vector<ScriptStep> steps;
    };

    struct TargetSpec
    {
            std::string                                      name;
            std::vector<std::string>                         argv;
            std::vector<std::pair<std::string, std::string>> env;
            MatchKind                                        match{ MatchKind::Pid };
            std::string                                      pattern;
            std::uint32_t                                    frames{ 1U };
            std::uint32_t interval_ms{ defaultTargetIntervalMs };
            std::uint32_t delay_ms{ defaultTargetSettleMs };
            double        timeout_s{ defaultTimeoutSeconds };
            bool          kill_after{ true };
    };

    struct BatchSection
    {
            std::filesystem::path output_root;
    };

    struct NotifySection
    {
            bool           enabled{ false };
            NotifyStrategy strategy{ NotifyStrategy::Os };
            std::uint32_t  popup_timeout_ms{ defaultPopupTimeoutMs };
    };

    struct CompareSection
    {
            CompareMode                          mode{ CompareMode::Rmse };
            double                               threshold{ defaultCompareThreshold };
            std::optional<std::filesystem::path> ref;
    };

    struct Config
    {
            std::filesystem::path        source;
            DefaultsSection              defaults;
            DisplaySection               display;
            std::optional<WatchSection>  watch;
            std::optional<ScriptSection> script;
            std::vector<TargetSpec>      targets;
            BatchSection                 batch;
            NotifySection                notifications;
            CompareSection               compare;
    };

    [[nodiscard]]
    grab::Result<Config>
    load( const std::filesystem::path& path );

    [[nodiscard]]
    grab::Result<std::vector<Config>>
    resolve( std::span<const std::string_view> explicit_paths );

}    // namespace grab::config
