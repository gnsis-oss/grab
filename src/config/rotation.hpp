#pragma once

#include "grab/config.hpp"
#include "grab/result.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace grab::config
{

    class RotationLedger
    {
        public:

            RotationLedger( std::filesystem::path dir,
                            std::string           pattern,
                            WatchLimits           limits );

            [[nodiscard]]
            grab::Result<void>
            scan();

            [[nodiscard]]
            grab::Result<void>
            adopt( const std::filesystem::path& file );

            [[nodiscard]]
            grab::Result<std::size_t>
            prune_age( std::chrono::system_clock::time_point now );

            [[nodiscard]]
            bool
            paused() const noexcept;

            [[nodiscard]]
            grab::Result<void>
            refresh_disk();

        private:

            enum class RemoveState : std::uint8_t
            {
                Removed,
                Dropped,
                Refreshed,
                Count,
            };

            struct LedgerFile
            {
                    std::filesystem::path           path;
                    std::filesystem::file_time_type modified_at;
                    std::uintmax_t                  size{};
            };

            std::filesystem::path   dir_;
            std::string             pattern_;
            WatchLimits             limits_;
            std::vector<LedgerFile> files_;
            std::uintmax_t          total_bytes_{};
            bool                    paused_{};

            void
            recalculate_total() noexcept;

            void
            update_pause_state() noexcept;

            [[nodiscard]]
            grab::Result<std::optional<LedgerFile>>
            inspect_regular_file( const std::filesystem::path& file ) const;

            [[nodiscard]]
            grab::Result<void>
            reconcile();

            [[nodiscard]]
            grab::Result<RemoveState>
            remove_file( std::size_t index );

            [[nodiscard]]
            grab::Result<void>
            enforce_max_files();
    };

}    // namespace grab::config
