#pragma once

// ┌──────────────────────────────────────────────────────────────────────────┐
// │  overlay_align — measure where overlay shapes ACTUALLY land.             │
// │                                                                          │
// │  The stage examples aim clicks in a11y space and draw their goal boxes   │
// │  and trails through the overlay. Those are different spaces with         │
// │  different owners, and on some desktops they do not coincide — the       │
// │  operator watched goal boxes sit offset from the elements they framed.   │
// │  Assuming identity is exactly the class of mistake this suite keeps      │
// │  paying for, so it is measured instead:                                  │
// │                                                                          │
// │    1. draw a small magenta probe at a chosen device position             │
// │    2. capture the display and find where the probe actually rendered    │
// │    3. derive scale and offset; remove the probe                          │
// │                                                                          │
// │  Every shape is then drawn through the inverse map, so what the viewer   │
// │  sees sits ON the element. Identity measures as identity and costs one   │
// │  probe flash; a failed measurement is reported and falls back to         │
// │  identity rather than drawing nothing.                                   │
// └──────────────────────────────────────────────────────────────────────────┘

#include "support/surface.hpp"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <grab/overlay.hpp>
#include <grab/screen.hpp>
#include <iostream>
#include <thread>

namespace ladder::view::align
{

    // Device rect -> the rect to hand the overlay so it RENDERS at the
    // device rect. Identity until measure() succeeds.
    struct OverlayMap
    {
            double sx_       = 1.0;
            double sy_       = 1.0;
            double dx_       = 0.0;
            double dy_       = 0.0;
            bool   measured_ = false;

            [[nodiscard]]
            double
            x( double device_x ) const noexcept
            {
                return ( device_x - dx_ ) / sx_;
            }

            [[nodiscard]]
            double
            y( double device_y ) const noexcept
            {
                return ( device_y - dy_ ) / sy_;
            }

            [[nodiscard]]
            view::ViewRect
            rect( const view::ViewRect& device ) const noexcept
            {
                return view::ViewRect{ .x_ = x( device.x_ ),
                                       .y_ = y( device.y_ ),
                                       .w_ = device.w_ / sx_,
                                       .h_ = device.h_ / sy_ };
            }

            [[nodiscard]]
            bool
            identity() const noexcept
            {
                return std::abs( sx_ - 1.0 ) < 0.01 && std::abs( sy_ - 1.0 ) < 0.01 &&
                       std::abs( dx_ ) < 1.5 && std::abs( dy_ ) < 1.5;
            }
    };

    // Draw a probe at overlay coordinates (probe_x, probe_y), find where it
    // rendered on the display, and derive the overlay->device affine. The
    // probe position should sit in a quiet region near where the run will
    // draw — over the browser window, clear of its chrome.
    [[nodiscard]]
    inline OverlayMap
    measure( grab::Overlay*          overlay,
             grab::Screen&           screen,
             grab::CoordinateSpaceId space,
             double                  probe_x,
             double                  probe_y )
    {
        OverlayMap map;
        if( overlay == nullptr )
        {
            return map;
        }
        constexpr double        probe_size = 24.0;
        // Full magenta, nearly opaque: a colour no page in this suite uses.
        constexpr std::uint8_t  probe_r    = 255U;
        constexpr std::uint8_t  probe_b    = 255U;
        constexpr int           settle_ms  = 150;
        constexpr std::uint8_t  tolerance  = 40U;

        auto added = overlay->add( grab::overlay::Shape{
            .geometry = grab::overlay::Rect{
                .bounds = grab::SpaceRect{ .x     = probe_x,
                                           .y     = probe_y,
                                           .w     = probe_size,
                                           .h     = probe_size,
                                           .space = space } },
            .stroke   = std::nullopt,
            .fill =
                grab::overlay::FillStyle{
                    .color = grab::overlay::Color{ .r = probe_r,
                                                   .g = 0U,
                                                   .b = probe_b,
                                                   .a = 250U } },
            .lifetime = grab::overlay::Persistent{},
            .band     = grab::overlay::Band::Annotation,
            .z        = 90,
        } );
        if( !added.has_value() )
        {
            std::cout << "  align     probe could not be drawn — assuming "
                         "identity\n";
            return map;
        }
        ( void )overlay->flush();
        std::this_thread::sleep_for( std::chrono::milliseconds{ settle_ms } );

        auto frame = screen.display();
        ( void )overlay->remove( *added );
        ( void )overlay->flush();
        if( !frame.has_value() || frame->empty() )
        {
            std::cout << "  align     capture failed — assuming identity\n";
            return map;
        }

        const std::uint32_t bpp = grab::bytes_per_pixel( frame->format );
        const bool          bgr = frame->format == grab::PixelFormat::Bgra ||
                         frame->format == grab::PixelFormat::Bgr;
        double        min_x = 1E9;
        double        min_y = 1E9;
        double        max_x = -1.0;
        double        max_y = -1.0;
        std::uint64_t hits  = 0U;
        for( std::uint32_t row = 0U; row < frame->height; ++row )
        {
            const std::size_t base = static_cast<std::size_t>( row ) * frame->stride;
            for( std::uint32_t col = 0U; col < frame->width; ++col )
            {
                const std::size_t at = base +
                                       ( static_cast<std::size_t>( col ) * bpp );
                if( at + 2U >= frame->pixels.size() )
                {
                    continue;
                }
                const auto b0 = static_cast<std::uint8_t>( frame->pixels[at] );
                const auto b1 = static_cast<std::uint8_t>( frame->pixels[at + 1U] );
                const auto b2 = static_cast<std::uint8_t>( frame->pixels[at + 2U] );
                const auto red   = bgr ? b2 : b0;
                const auto green = b1;
                const auto blue  = bgr ? b0 : b2;
                if( red >= probe_r - tolerance && blue >= probe_b - tolerance &&
                    green <= tolerance )
                {
                    min_x = std::min( min_x, static_cast<double>( col ) );
                    min_y = std::min( min_y, static_cast<double>( row ) );
                    max_x = std::max( max_x, static_cast<double>( col ) );
                    max_y = std::max( max_y, static_cast<double>( row ) );
                    ++hits;
                }
            }
        }
        // The probe must dominate its own bounding box, or the box belongs to
        // something else magenta on screen.
        const double found_w = max_x - min_x + 1.0;
        const double found_h = max_y - min_y + 1.0;
        const bool   sane    = hits > 0U && found_w >= probe_size * 0.5 &&
                          found_w <= probe_size * 4.0 &&
                          found_h >= probe_size * 0.5 &&
                          found_h <= probe_size * 4.0 &&
                          static_cast<double>( hits ) >= 0.5 * found_w * found_h;
        if( !sane )
        {
            std::cout << "  align     probe not found on screen (hits=" << hits
                      << ") — assuming identity\n";
            return map;
        }
        // Quantize: antialiasing at the probe's edge can eat a boundary
        // pixel, and 23/24 read as a 4% scale would INTRODUCE drift far from
        // the probe while looking corrected next to it. A size within a few
        // pixels of authored is scale one, exactly; only a substantial
        // difference is a real scale.
        constexpr double size_slack = 3.0;
        map.sx_ = std::abs( found_w - probe_size ) <= size_slack
                      ? 1.0
                      : found_w / probe_size;
        map.sy_ = std::abs( found_h - probe_size ) <= size_slack
                      ? 1.0
                      : found_h / probe_size;
        map.dx_       = min_x - ( map.sx_ * probe_x );
        map.dy_       = min_y - ( map.sy_ * probe_y );
        map.measured_ = true;
        if( map.identity() )
        {
            std::cout << "  align     overlay == display (identity, measured)\n";
            map.sx_ = 1.0;
            map.sy_ = 1.0;
            map.dx_ = 0.0;
            map.dy_ = 0.0;
        }
        else
        {
            std::cout << "  align     overlay -> display scale (" << map.sx_ << ","
                      << map.sy_ << ") offset (" << static_cast<int>( map.dx_ )
                      << "," << static_cast<int>( map.dy_ )
                      << ") — correcting every shape\n";
        }
        return map;
    }

}    // namespace ladder::view::align
