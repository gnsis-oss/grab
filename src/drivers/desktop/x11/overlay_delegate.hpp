#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/overlay.hpp"
#include "grab/result.hpp"
#include "grab/space.hpp"
#include "spi/overlay_delegate.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <xcb/xcb.h>

namespace grab::core
{

    class Reactor;

}

namespace grab::drivers::desktop::x11
{

    namespace detail
    {

        struct OverlayProbePrerequisites
        {
                bool argb32_visual{};
                bool xfixes_shape_input{};
                bool compositor_owner{};
        };

        struct OverlayDamagePlan
        {
                bool                                     render_frame{};
                bool                                     continue_fade{};
                std::optional<std::chrono::milliseconds> next_lifetime_deadline;
        };

        [[nodiscard]]
        std::optional<std::string_view>
        overlay_probe_reason( OverlayProbePrerequisites prerequisites ) noexcept;

        [[nodiscard]]
        OverlayDamagePlan
        overlay_damage_plan( std::span<const overlay::ShapeRecord> shapes,
                             std::chrono::milliseconds             now,
                             bool scene_dirty ) noexcept;

        [[nodiscard]]
        Result<void>
        apply_input_passthrough( xcb_connection_t* connection,
                                 xcb_window_t      window );

        class X11OverlayDelegateTestAccess;

    }    // namespace detail

    class X11OverlayDelegate final : public spi::OverlayDelegate
    {
        public:

            using AvailabilityChanged = std::function<void( bool, const Error* )>;
            using TopologyRefresh     = std::function<Result<void>()>;

            [[nodiscard]]
            static Result<std::unique_ptr<X11OverlayDelegate>>
            create( core::Reactor*   reactor = nullptr,
                    std::string_view display = {} );

            [[nodiscard]]
            static Result<void>
            probe( std::string_view display = {} );

            ~X11OverlayDelegate() override;

            X11OverlayDelegate( const X11OverlayDelegate& ) = delete;
            X11OverlayDelegate&
            operator=( const X11OverlayDelegate& )     = delete;
            X11OverlayDelegate( X11OverlayDelegate&& ) = delete;
            X11OverlayDelegate&
            operator=( X11OverlayDelegate&& ) = delete;

            [[nodiscard]]
            Result<void>
            open( CoordinateSpaceId space ) override;

            [[nodiscard]]
            Result<void>
            apply( std::span<const overlay::SceneDelta> deltas ) override;

            [[nodiscard]]
            Result<void>
            resync( const overlay::SceneSnapshot& scene ) override;

            [[nodiscard]]
            Result<void>
            flush( overlay::Revision through ) override;

            void
            close() override;

            void
            set_availability_changed( AvailabilityChanged callback );

            void
            set_topology_refresh( TopologyRefresh callback );

        private:

            friend class detail::X11OverlayDelegateTestAccess;

            class Impl;

            explicit X11OverlayDelegate( std::shared_ptr<Impl> impl ) noexcept;

            std::shared_ptr<Impl> impl_;
    };

    namespace detail
    {

        // Narrow test seam: exercises window recreation and ShapeInput without
        // weakening the production compositor gate or mapping a bare-Xvfb window.
        class X11OverlayDelegateTestAccess final
        {
            public:

                [[nodiscard]]
                static Result<void>
                open_unmapped( X11OverlayDelegate& delegate,
                               CoordinateSpaceId   space );

                [[nodiscard]]
                static xcb_window_t
                window( const X11OverlayDelegate& delegate ) noexcept;
        };

    }    // namespace detail

}    // namespace grab::drivers::desktop::x11
