#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "drivers/desktop/x11/coordinate_authority.hpp"
#include "grab/capture.hpp"
#include "grab/result.hpp"
#include "grab/space.hpp"
#include "screen/x11_capture.hpp"

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace grab::drivers::desktop::x11
{

    class X11CaptureRoute final
    {
        public:

            [[nodiscard]]
            static grab::Result<X11CaptureRoute>
            open( const char* display = nullptr );

            X11CaptureRoute( const X11CaptureRoute& ) = delete;
            X11CaptureRoute&
            operator=( const X11CaptureRoute& )           = delete;
            X11CaptureRoute( X11CaptureRoute&& ) noexcept = default;
            X11CaptureRoute&
            operator=( X11CaptureRoute&& ) noexcept = default;

            [[nodiscard]]
            grab::Result<grab::Frame>
            capture_output( std::string_view name );

            [[nodiscard]]
            grab::Result<grab::Frame>
            capture_window( std::uint32_t window );

            [[nodiscard]]
            grab::Result<std::vector<grab::TransformRecord>>
            refresh_transforms();

            [[nodiscard]]
            const CoordinateAuthority&
            coordinate_authority() const noexcept;

            [[nodiscard]]
            std::shared_ptr<const grab::detail::SpaceGraph>
            graph() const noexcept;

            [[nodiscard]]
            grab::CoordinateSpaceId
            global_space() const noexcept;

        private:

            X11CaptureRoute( grab::screen::X11Capturer capturer,
                             CoordinateAuthority       authority ) noexcept;

            grab::screen::X11Capturer capturer_;
            CoordinateAuthority       authority_;
    };

}    // namespace grab::drivers::desktop::x11
