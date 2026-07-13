#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/ids.hpp"
#include "grab/presentation.hpp"
#include "grab/result.hpp"
#include "grab/ui.hpp"
#include "kernel/graph/target_registry.hpp"
#include "screen/enumerate.hpp"
#include "spi/tree_source.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include <xcb/xcb.h>

namespace grab::drivers::desktop::x11
{

    class X11TreeSource final : public grab::spi::TreeSource
    {
        public:

            using WindowEnumerator =
                std::function<grab::Result<std::vector<grab::screen::WindowInfo>>()>;

            X11TreeSource( grab::RuntimeId         runtime,
                           grab::DisplayGeneration display_generation,
                           xcb_connection_t*       connection,
                           xcb_window_t            root );

            X11TreeSource( grab::RuntimeId         runtime,
                           grab::DisplayGeneration display_generation,
                           WindowEnumerator        enumerate_windows );

            [[nodiscard]]
            grab::Result<grab::UiSnapshot>
            snapshot( std::uint32_t                 tree,
                      const grab::OperationContext& context ) override;

            [[nodiscard]]
            grab::Result<std::optional<grab::spi::UiUpdate>>
            next_update( const grab::OperationContext& context ) override;

            [[nodiscard]]
            const grab::kernel::TargetRegistry&
            target_registry() const noexcept;

            [[nodiscard]]
            grab::Result<xcb_window_t>
            resolve_xid( const grab::WidgetRef& widget ) const;

        private:

            struct WindowBinding
            {
                    grab::NodeId           node{};
                    grab::kernel::TargetId target{};
                    grab::SurfaceId        surface{};
            };

            [[nodiscard]]
            grab::Result<WindowBinding>
            observe_window( const grab::screen::WindowInfo& window,
                            const grab::SpaceRect&          bounds );

            [[nodiscard]]
            grab::Result<void>
            retire_missing_windows( const std::set<std::uint32_t>& active_xids );

            [[nodiscard]]
            grab::kernel::AliasEdge
            alias_for( std::uint32_t xid ) const;

            [[nodiscard]]
            grab::UiNodeRecord
            node_record( const grab::screen::WindowInfo& window,
                         const WindowBinding&            binding,
                         const grab::SpaceRect&          bounds,
                         std::uint64_t                   revision ) const;

            static constexpr std::uint32_t         firstTree    = 1U;
            static constexpr std::uint32_t         firstEpoch   = 1U;
            static constexpr std::uint64_t         firstNode    = 0X1'00'00'00'00ULL;
            static constexpr std::uint64_t         firstSurface = 1U;
            static constexpr std::uint32_t         rootSpace    = 1U;

            grab::RuntimeId                        runtime_{};
            grab::DisplayGeneration                display_generation_{};
            WindowEnumerator                       enumerate_windows_;
            grab::kernel::TargetRegistry           targets_;
            std::map<std::uint32_t, WindowBinding> bindings_;
            std::uint64_t                          next_node_{ firstNode };
            std::uint64_t                          next_surface_{ firstSurface };
            std::uint64_t                          revision_{};
            std::string                            alias_authority_;
            mutable std::mutex                     mutex_;
    };

}    // namespace grab::drivers::desktop::x11
