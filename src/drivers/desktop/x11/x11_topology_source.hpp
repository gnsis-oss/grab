#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "drivers/desktop/x11/enumerate.hpp"
#include "grab/result.hpp"
#include "spi/topology_source.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace grab::drivers::desktop::x11
{

    struct TopologyRecord
    {
            std::vector<grab::screen::OutputInfo> outputs;
            std::uint64_t                         generation = 0U;
            bool                                  changed    = false;
    };

    class X11TopologySource final : public grab::spi::TopologySource
    {
        public:

            using RefreshHook = std::function<void()>;

            explicit X11TopologySource( RefreshHook on_change = {},
                                        const char* display   = nullptr );

            // RandR polling (event subscription is a deferred stretch goal):
            // each poll() re-enumerates outputs and compares against the last
            // observed set; on a change it bumps the generation and invokes the
            // refresh hook, which feeds CoordinateAuthority::refresh.
            [[nodiscard]]
            grab::Result<TopologyRecord>
            poll();

        private:

            RefreshHook                           on_change_;
            std::string                           display_;
            bool                                  use_default_display_{ true };
            std::vector<grab::screen::OutputInfo> last_outputs_;
            std::uint64_t                         generation_{};
            bool                                  has_baseline_{};
    };

}    // namespace grab::drivers::desktop::x11
