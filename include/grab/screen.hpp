#pragma once

#include "grab/geometry/rectangle.hpp"
#include "grab/image.hpp"
#include "grab/result.hpp"
#include "grab/window_info.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace grab
{

    class Screen
    {
        public:

            [[nodiscard]]
            static grab::Result<Screen>
            open( const char* display = nullptr );

            ~Screen();

            Screen( const Screen& ) = delete;
            Screen&
            operator=( const Screen& ) = delete;
            Screen( Screen&& other ) noexcept;
            Screen&
            operator=( Screen&& other ) noexcept;

            [[nodiscard]]
            grab::Result<Image>
            window_by_class( const std::vector<std::string>& wm_class_candidates );

            [[nodiscard]]
            grab::Result<Image>
            display();

            [[nodiscard]]
            grab::Result<Image>
            region( std::int16_t  x,
                    std::int16_t  y,
                    std::uint16_t width,
                    std::uint16_t height );

            [[nodiscard]]
            grab::Result<Image>
            active_window();

            // Snapshot of every mapped top-level window, in the window manager's
            // client-list order. Succeeds with an empty vector on a bare display
            // that has no managed clients.
            [[nodiscard]]
            grab::Result<std::vector<WindowSummary>>
            windows();

            // Raises and focuses the first window whose WM_CLASS contains any of
            // the candidates (case-insensitively), and returns that window as it
            // was seen before activation. Fails with WindowNotFound when nothing
            // matches. Activation is a request to the window manager: it is
            // asynchronous, and a WM may legitimately refuse or defer it, so the
            // success of this call does not by itself prove the window is focused.
            [[nodiscard]]
            grab::Result<WindowSummary>
            activate_window_by_class(
                const std::vector<std::string>& wm_class_candidates
            );

            // Captures one window by its native id, as reported by windows(). The
            // disambiguation escape hatch for applications that own several
            // windows sharing a WM_CLASS.
            [[nodiscard]]
            grab::Result<Image>
            window_by_id( std::uint32_t window_id );

            // Raises and focuses one window by its native id. Same asynchronous
            // caveat as activate_window_by_class.
            [[nodiscard]]
            grab::Result<void>
            activate_window( std::uint32_t window_id );

            // Default budget for place_window to reach and hold the requested
            // geometry. Generous because a window manager may animate an
            // un-maximise before it honours the new geometry.
            static constexpr std::chrono::milliseconds defaultPlacementTimeout{ 3'000 };

            // Drives a window to exactly `request` — position and size in the same
            // screen coordinates windows() reports — and returns the geometry
            // actually reached.
            //
            // Clears maximised/fullscreen state first, because a window manager
            // silently ignores geometry requests against a maximised window, then
            // applies the geometry and polls until the window has *held* the
            // request across several consecutive reads. That settling requirement
            // is the point of the call: window managers routinely resize a window
            // asynchronously just after it maps, so a single post-request read
            // proves nothing.
            //
            // Fails with DeadlineExceeded, naming the geometry last observed, if
            // the window never settles on the request. It never reports partial
            // success: callers use this to derive click coordinates, where a wrong
            // answer is worse than a refusal.
            [[nodiscard]]
            grab::Result<grab::geometry::Rectangle>
            place_window( std::uint32_t                    window_id,
                          const grab::geometry::Rectangle& request,
                          std::chrono::milliseconds timeout = defaultPlacementTimeout );

        private:

            struct Impl;

            explicit Screen( std::unique_ptr<Impl> impl ) noexcept;

            std::unique_ptr<Impl> impl_;
    };

}    // namespace grab
