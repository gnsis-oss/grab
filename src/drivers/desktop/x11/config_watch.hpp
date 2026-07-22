#pragma once

#include "grab/config.hpp"
#include "grab/result.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace grab::screen
{

    struct WatchStats
    {
            std::uint64_t captured{};
            std::uint64_t errors{};
            std::uint64_t skipped{};
            bool          paused{};
            bool          script_failed{};
            std::string   last_capture;
    };

    class ConfigWatcher
    {
        public:

            [[nodiscard]]
            static grab::Result<ConfigWatcher>
            start( const grab::config::Config& cfg );

            ~ConfigWatcher();

            ConfigWatcher( const ConfigWatcher& ) = delete;
            ConfigWatcher&
            operator=( const ConfigWatcher& ) = delete;
            ConfigWatcher( ConfigWatcher&& other ) noexcept;
            ConfigWatcher&
            operator=( ConfigWatcher&& other ) noexcept;

            void
            stop();

            [[nodiscard]]
            WatchStats
            stats() const;

        private:

            class Impl;

            explicit ConfigWatcher( std::unique_ptr<Impl> impl ) noexcept;

            std::unique_ptr<Impl> impl_;
    };

}    // namespace grab::screen
