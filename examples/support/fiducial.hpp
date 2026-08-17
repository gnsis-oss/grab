#pragma once

// ┌──────────────────────────────────────────────────────────────────────────┐
// │  fiducial — anchor every coordinate to the RENDERED PIXELS.              │
// │                                                                          │
// │  Everything geometric in the stage examples used to come from AT-SPI     │
// │  rects. On some desktops those rects are offset (or scaled) from where   │
// │  the elements actually render, and no amount of window-manager           │
// │  arithmetic fixes a source that is itself wrong. The pixels cannot be    │
// │  wrong — they are what the operator sees — so the pages author two       │
// │  small colour fiducials in opposite corners (position:fixed, so they     │
// │  never scroll), and each run measures the a11y -> device map by          │
// │  comparing where the fiducials SAY they are (their a11y rects) with      │
// │  where they ACTUALLY render (their colour bboxes in a capture).          │
// │                                                                          │
// │  The map is applied inside every resolve, so every aim, visibility       │
// │  check and overlay shape downstream is pixel-anchored. Identity          │
// │  measures as identity; anything else is corrected and reported; a        │
// │  failed measurement falls back to identity, loudly. The window manager   │
// │  is never part of the geometry path.                                     │
// └──────────────────────────────────────────────────────────────────────────┘

#include "support/surface.hpp"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <grab/locator.hpp>
#include <grab/role.hpp>
#include <grab/screen.hpp>
#include <grab/session.hpp>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace ladder::view::fid
{

    // Authored fiducial geometry and colours. Colours are chosen to collide
    // with nothing else in the suite: not the page palettes, not the overlay
    // cyan/amber/green, not the overlay-alignment probe's magenta.
    constexpr int          patch_px = 18;
    constexpr const char*  fida_css = "#ff0066";    // top-left
    constexpr const char*  fidb_css = "#66ff00";    // bottom-right
    constexpr std::uint8_t fida_r = 255U, fida_g = 0U, fida_b = 102U;
    constexpr std::uint8_t fidb_r = 102U, fidb_g = 255U, fidb_b = 0U;

    // The two patches, for a page's <body>. position:fixed, opposite
    // corners: always on screen, never scrolled, and far enough apart that
    // the fit is well conditioned on both axes. pointer-events:none so they
    // can never swallow a click.
    [[nodiscard]]
    inline std::string
    patches_html()
    {
        const auto patch = []( const char* name, const char* css,
                               const char* corner )
        {
            return std::string{ "<button aria-label=\"" } + name +
                   "\" tabindex=\"-1\" style=\"position:fixed;" + corner +
                   "width:" + std::to_string( patch_px ) +
                   "px;height:" + std::to_string( patch_px ) +
                   "px;background:" + css +
                   ";border:0;padding:0;pointer-events:none;\"></button>\n";
        };
        return patch( "FIDA", fida_css, "left:2px;top:2px;" ) +
               patch( "FIDB", fidb_css, "right:2px;bottom:2px;" );
    }

    // a11y -> device, per axis. Identity until measured.
    struct SpaceMap
    {
            double sx_       = 1.0;
            double sy_       = 1.0;
            double dx_       = 0.0;
            double dy_       = 0.0;
            bool   measured_ = false;

            [[nodiscard]]
            view::ViewRect
            rect( const view::ViewRect& a11y ) const noexcept
            {
                return view::ViewRect{ .x_ = ( sx_ * a11y.x_ ) + dx_,
                                       .y_ = ( sy_ * a11y.y_ ) + dy_,
                                       .w_ = sx_ * a11y.w_,
                                       .h_ = sy_ * a11y.h_ };
            }

            [[nodiscard]]
            bool
            identity() const noexcept
            {
                return std::abs( sx_ - 1.0 ) < 0.005 &&
                       std::abs( sy_ - 1.0 ) < 0.005 && std::abs( dx_ ) < 1.5 &&
                       std::abs( dy_ ) < 1.5;
            }
    };

    // The process-wide map, applied by every resolve helper. One example is
    // one process, so a single mutable map is the whole state.
    [[nodiscard]]
    inline SpaceMap&
    current() noexcept
    {
        static SpaceMap map;
        return map;
    }

    namespace detail
    {

        struct Found
        {
                double cx_ = 0.0;
                double cy_ = 0.0;
                double w_  = 0.0;
                double h_  = 0.0;
        };

        [[nodiscard]]
        inline std::optional<Found>
        locate( const grab::Image& frame,
                std::uint8_t       want_r,
                std::uint8_t       want_g,
                std::uint8_t       want_b )
        {
            constexpr std::uint8_t tol = 36U;
            const std::uint32_t    bpp = grab::bytes_per_pixel( frame.format );
            const bool             bgr = frame.format == grab::PixelFormat::Bgra ||
                             frame.format == grab::PixelFormat::Bgr;
            double        min_x = 1E9;
            double        min_y = 1E9;
            double        max_x = -1.0;
            double        max_y = -1.0;
            std::uint64_t hits  = 0U;
            for( std::uint32_t row = 0U; row < frame.height; ++row )
            {
                const std::size_t base = static_cast<std::size_t>( row ) *
                                         frame.stride;
                for( std::uint32_t col = 0U; col < frame.width; ++col )
                {
                    const std::size_t at =
                        base + ( static_cast<std::size_t>( col ) * bpp );
                    if( at + 2U >= frame.pixels.size() )
                    {
                        continue;
                    }
                    const auto b0 = static_cast<std::uint8_t>( frame.pixels[at] );
                    const auto b1 =
                        static_cast<std::uint8_t>( frame.pixels[at + 1U] );
                    const auto b2 =
                        static_cast<std::uint8_t>( frame.pixels[at + 2U] );
                    const auto red   = bgr ? b2 : b0;
                    const auto green = b1;
                    const auto blue  = bgr ? b0 : b2;
                    const auto close = [&]( std::uint8_t have, std::uint8_t want )
                    {
                        return have >= ( want > tol ? want - tol : 0 ) &&
                               have <= ( want < 255 - tol ? want + tol : 255 );
                    };
                    if( close( red, want_r ) && close( green, want_g ) &&
                        close( blue, want_b ) )
                    {
                        min_x = std::min( min_x, static_cast<double>( col ) );
                        min_y = std::min( min_y, static_cast<double>( row ) );
                        max_x = std::max( max_x, static_cast<double>( col ) );
                        max_y = std::max( max_y, static_cast<double>( row ) );
                        ++hits;
                    }
                }
            }
            const double w = max_x - min_x + 1.0;
            const double h = max_y - min_y + 1.0;
            const double size = static_cast<double>( patch_px );
            const bool   sane = hits > 0U && w >= size * 0.5 && w <= size * 4.0 &&
                              h >= size * 0.5 && h <= size * 4.0 &&
                              static_cast<double>( hits ) >= 0.5 * w * h;
            if( !sane )
            {
                return std::nullopt;
            }
            return Found{ .cx_ = ( min_x + max_x ) / 2.0,
                          .cy_ = ( min_y + max_y ) / 2.0,
                          .w_  = w,
                          .h_  = h };
        }

        [[nodiscard]]
        inline std::optional<view::ViewRect>
        a11y_rect_of( grab::Session&   session,
                      std::string_view name )
        {
            auto matches = session.resolve_all( grab::sel::role( grab::role::button ) );
            if( !matches.has_value() )
            {
                return std::nullopt;
            }
            for( const grab::Match& match : *matches )
            {
                auto described = session.describe( match );
                if( !described.has_value() )
                {
                    continue;
                }
                const auto& info = *described;
                if( info.name == name && info.bounds.w > 0.0 && info.bounds.h > 0.0 )
                {
                    return view::ViewRect{ .x_ = info.bounds.x,
                                           .y_ = info.bounds.y,
                                           .w_ = info.bounds.w,
                                           .h_ = info.bounds.h };
                }
            }
            return std::nullopt;
        }

    }    // namespace detail

    // Resolve both fiducials through a11y, locate both in a capture, fit the
    // per-axis affine, install it as current(). Polls until the page's tree
    // is up. Falls back to identity — loudly — rather than refusing: the
    // a11y rects are still the best available guess when the fit fails, and
    // downstream gates (second-anchor verification, press-inside reads)
    // remain in force.
    inline void
    measure( grab::Session& session,
             grab::Screen&  screen )
    {
        constexpr int poll_ms    = 200;
        constexpr int poll_tries = 150;
        SpaceMap&     map        = current();
        map                      = SpaceMap{};

        std::optional<view::ViewRect> fida;
        std::optional<view::ViewRect> fidb;
        for( int attempt = 0; attempt < poll_tries; ++attempt )
        {
            if( auto synced = session.resync(); synced.has_value() )
            {
                fida = detail::a11y_rect_of( session, "FIDA" );
                fidb = detail::a11y_rect_of( session, "FIDB" );
                if( fida.has_value() && fidb.has_value() )
                {
                    break;
                }
            }
            std::this_thread::sleep_for( std::chrono::milliseconds{ poll_ms } );
        }
        if( !fida.has_value() || !fidb.has_value() )
        {
            std::cout << "  space     fiducials never resolved — geometry stays "
                         "on raw a11y rects\n";
            return;
        }

        auto frame = screen.display();
        if( !frame.has_value() || frame->empty() )
        {
            std::cout << "  space     capture failed — geometry stays on raw "
                         "a11y rects\n";
            return;
        }
        const auto found_a = detail::locate( *frame, fida_r, fida_g, fida_b );
        const auto found_b = detail::locate( *frame, fidb_r, fidb_g, fidb_b );
        if( !found_a.has_value() || !found_b.has_value() )
        {
            std::cout << "  space     fiducial pixels not found ("
                      << ( found_a.has_value() ? "B" : "A" )
                      << " missing) — geometry stays on raw a11y rects\n";
            return;
        }

        const double a11y_ax  = fida->center_x();
        const double a11y_ay  = fida->center_y();
        const double a11y_bx  = fidb->center_x();
        const double a11y_by  = fidb->center_y();
        const double span_x   = a11y_bx - a11y_ax;
        const double span_y   = a11y_by - a11y_ay;
        constexpr double min_span = 100.0;
        if( std::abs( span_x ) < min_span || std::abs( span_y ) < min_span )
        {
            std::cout << "  space     fiducial span degenerate — geometry stays "
                         "on raw a11y rects\n";
            return;
        }
        double sx = ( found_b->cx_ - found_a->cx_ ) / span_x;
        double sy = ( found_b->cy_ - found_a->cy_ ) / span_y;
        // Quantize: sub-pixel measurement slop must not masquerade as scale.
        if( std::abs( sx - 1.0 ) < 0.01 )
        {
            sx = 1.0;
        }
        if( std::abs( sy - 1.0 ) < 0.01 )
        {
            sy = 1.0;
        }
        constexpr double sane_low  = 0.5;
        constexpr double sane_high = 3.0;
        if( sx < sane_low || sx > sane_high || sy < sane_low || sy > sane_high )
        {
            std::cout << "  space     fit implausible (scale " << sx << "," << sy
                      << ") — geometry stays on raw a11y rects\n";
            return;
        }
        map.sx_       = sx;
        map.sy_       = sy;
        map.dx_       = found_a->cx_ - ( sx * a11y_ax );
        map.dy_       = found_a->cy_ - ( sy * a11y_ay );
        map.measured_ = true;
        if( map.identity() )
        {
            map = SpaceMap{};
            map.measured_ = true;
            std::cout << "  space     a11y == pixels (identity, measured from "
                         "fiducials)\n";
        }
        else
        {
            std::cout << "  space     a11y -> pixels scale (" << map.sx_ << ","
                      << map.sy_ << ") offset (" << static_cast<int>( map.dx_ )
                      << "," << static_cast<int>( map.dy_ )
                      << ") — every rect corrected onto the rendered page\n";
        }
    }

}    // namespace ladder::view::fid
