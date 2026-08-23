#pragma once

// Pixel-space helpers shared by the stage rungs: reading a region's mean
// colour out of a capture, comparing two colours as a distance, and writing
// a frame to disk. Extracted from stage_button.cpp when stage_drag arrived —
// two copies of a pixel-format branch is two chances to get the channel
// order wrong in only one of them.

#include "support/surface.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <grab/screen.hpp>
#include <optional>

namespace ladder::view::pixel
{

    struct Rgb
    {
            double r_ = 0.0;
            double g_ = 0.0;
            double b_ = 0.0;
    };

    // Mean colour of a rect in a capture. Compared as a distance rather than
    // for equality: a real browser antialiases, and the question is whether a
    // region changed, not whether it matches a hex string exactly.
    [[nodiscard]]
    inline std::optional<Rgb>
    mean_colour( const grab::Image&    frame,
                 const view::ViewRect& rect )
    {
        if( frame.empty() )
        {
            return std::nullopt;
        }
        const std::uint32_t bpp = grab::bytes_per_pixel( frame.format );
        const bool          bgr = frame.format ==
                                  grab::PixelFormat::Bgra ||
                                  frame.format == grab::PixelFormat::Bgr;

        const auto          x0  = static_cast<std::uint32_t>( std::max( 0.0, rect.x_ ) );
        const auto          y0  = static_cast<std::uint32_t>( std::max( 0.0, rect.y_ ) );
        const auto x1 = static_cast<std::uint32_t>( std::max( 0.0, rect.x_ + rect.w_ ) );
        const auto y1 = static_cast<std::uint32_t>( std::max( 0.0, rect.y_ + rect.h_ ) );

        double     red     = 0.0;
        double     grn     = 0.0;
        double     blu     = 0.0;
        std::uint64_t seen = 0U;
        for( std::uint32_t row = y0; row < y1 && row < frame.height; ++row )
        {
            for( std::uint32_t col = x0; col < x1 && col < frame.width; ++col )
            {
                const std::size_t at =
                    ( static_cast<std::size_t>( row ) * frame.stride ) +
                    ( static_cast<std::size_t>( col ) * bpp );
                if( at + 2U >= frame.pixels.size() )
                {
                    continue;
                }
                const auto b0 =
                    static_cast<double>( static_cast<std::uint8_t>( frame.pixels[at] ) );
                const auto b1 = static_cast<double>(
                    static_cast<std::uint8_t>( frame.pixels[at + 1U] )
                );
                const auto b2 = static_cast<double>(
                    static_cast<std::uint8_t>( frame.pixels[at + 2U] )
                );
                red += bgr ? b2 : b0;
                grn += b1;
                blu += bgr ? b0 : b2;
                ++seen;
            }
        }
        if( seen == 0U )
        {
            return std::nullopt;
        }
        const auto count = static_cast<double>( seen );
        return Rgb{ .r_ = red / count, .g_ = grn / count, .b_ = blu / count };
    }

    [[nodiscard]]
    inline double
    distance( const Rgb& lhs,
              const Rgb& rhs )
    {
        const double d_r = lhs.r_ - rhs.r_;
        const double d_g = lhs.g_ - rhs.g_;
        const double d_b = lhs.b_ - rhs.b_;
        return std::sqrt( ( d_r * d_r ) + ( d_g * d_g ) + ( d_b * d_b ) );
    }

    // One frame to disk as P6. One copy of the pixel-format branch.
    inline void
    write_ppm( const grab::Image&           frame,
               const std::filesystem::path& path )
    {
        std::ofstream shot( path, std::ios::binary );
        shot << "P6\n" << frame.width << " " << frame.height << "\n255\n";
        const std::uint32_t bpp = grab::bytes_per_pixel( frame.format );
        const bool          bgr = frame.format ==
                                  grab::PixelFormat::Bgra ||
                                  frame.format == grab::PixelFormat::Bgr;
        for( std::uint32_t row = 0U; row < frame.height; ++row )
        {
            for( std::uint32_t col = 0U; col < frame.width; ++col )
            {
                const std::size_t at =
                    ( static_cast<std::size_t>( row ) * frame.stride ) +
                    ( static_cast<std::size_t>( col ) * bpp );
                const auto b0 = static_cast<char>( frame.pixels[at] );
                const auto b1 = static_cast<char>( frame.pixels[at + 1U] );
                const auto b2 = static_cast<char>( frame.pixels[at + 2U] );
                shot.put( bgr ? b2 : b0 );
                shot.put( b1 );
                shot.put( bgr ? b0 : b2 );
            }
        }
    }

}    // namespace ladder::view::pixel
