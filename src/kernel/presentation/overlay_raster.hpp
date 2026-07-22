#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/geometry/rectangle.hpp"
#include "grab/geometry/size.hpp"
#include "grab/image.hpp"
#include "grab/overlay.hpp"
#include "grab/result.hpp"

#include <chrono>
#include <memory>
#include <span>
#include <vector>

namespace grab::kernel::presentation
{

    struct RasterFrame
    {
            // PixelFormat::Bgra with premultiplied channels. On little-endian X11
            // this is ARGB32 memory order: B, G, R, A bytes.
            // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
            const Image&                     pixels;
            std::vector<geometry::Rectangle> damage;
    };

    class OverlayRaster final
    {
        public:

            [[nodiscard]]
            static Result<OverlayRaster>
            create( geometry::Size size );

            ~OverlayRaster();

            OverlayRaster( const OverlayRaster& ) = delete;
            OverlayRaster&
            operator=( const OverlayRaster& ) = delete;
            OverlayRaster( OverlayRaster&& ) noexcept;
            OverlayRaster&
            operator=( OverlayRaster&& ) noexcept;

            [[nodiscard]]
            Result<RasterFrame>
            render( std::span<const overlay::ShapeRecord> shapes,
                    std::chrono::milliseconds             now );

            [[nodiscard]]
            geometry::Size
            size() const noexcept;

        private:

            struct Impl;

            explicit OverlayRaster( std::unique_ptr<Impl> impl ) noexcept;

            std::unique_ptr<Impl> impl_;
    };

}    // namespace grab::kernel::presentation
