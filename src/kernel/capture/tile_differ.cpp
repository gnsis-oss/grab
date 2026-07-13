#include "kernel/capture/tile_differ.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>

namespace grab::kernel
{
    namespace
    {

        [[nodiscard]]
        TileDiffResult
        full_invalidation( std::string reason )
        {
            return TileDiffResult{
                .kind        = TileDiffKind::FullInvalidation,
                .dirty_tiles = {},
                .reason      = std::move( reason ),
            };
        }

        [[nodiscard]]
        bool
        has_valid_layout( const Image& image )
        {
            const auto row_bytes = static_cast<std::size_t>( image.width ) *
                                   bytes_per_pixel( image.format );
            if( static_cast<std::size_t>( image.stride ) < row_bytes )
            {
                return false;
            }
            if( image.height == 0U )
            {
                return true;
            }

            const auto     last_row = static_cast<std::size_t>( image.height - 1U );
            const auto     stride   = static_cast<std::size_t>( image.stride );
            constexpr auto maximum  = std::numeric_limits<std::size_t>::max();
            if( stride != 0U && last_row > ( maximum - row_bytes ) / stride )
            {
                return false;
            }
            return ( last_row * stride ) + row_bytes <= image.pixels.size();
        }

        [[nodiscard]]
        bool
        tile_changed( const Image&  previous,
                      const Image&  current,
                      std::uint32_t x,
                      std::uint32_t y,
                      std::uint32_t width,
                      std::uint32_t height )
        {
            const auto bytes_per_pixel_value = bytes_per_pixel( current.format );
            const auto byte_x = static_cast<std::size_t>( x ) * bytes_per_pixel_value;
            const auto byte_width =
                static_cast<std::size_t>( width ) * bytes_per_pixel_value;
            const std::span<const std::byte> previous_pixels{ previous.pixels };
            const std::span<const std::byte> current_pixels{ current.pixels };

            for( std::uint32_t row = 0U; row < height; ++row )
            {
                const auto previous_offset =
                    ( static_cast<std::size_t>( y + row ) * previous.stride ) + byte_x;
                const auto current_offset =
                    ( static_cast<std::size_t>( y + row ) * current.stride ) + byte_x;
                const auto previous_row =
                    previous_pixels.subspan( previous_offset, byte_width );
                const auto current_row =
                    current_pixels.subspan( current_offset, byte_width );
                if( !std::ranges::equal( previous_row, current_row ) )
                {
                    return true;
                }
            }
            return false;
        }

    }    // namespace

    TileDiffResult
    TileDiffer::diff( const Image&   previous,
                      const Image&   current,
                      geometry::Size tile_size ) const
    {
        if( previous.width !=
            current.width ||
            previous.height !=
            current.height ||
            previous.format != current.format )
        {
            return full_invalidation( "image geometry or pixel format changed" );
        }
        if( tile_size.width == 0U || tile_size.height == 0U )
        {
            return full_invalidation( "tile dimensions must be non-zero" );
        }
        constexpr auto maximum_coordinate =
            static_cast<std::uint32_t>( std::numeric_limits<std::int32_t>::max() );
        if( current.width > maximum_coordinate || current.height > maximum_coordinate )
        {
            return full_invalidation( "image dimensions exceed rectangle coordinates" );
        }
        if( !has_valid_layout( previous ) || !has_valid_layout( current ) )
        {
            return full_invalidation(
                "image storage does not contain its active pixels"
            );
        }

        TileDiffResult result;
        for( std::uint32_t y = 0U; y < current.height; )
        {
            const auto height = std::min( tile_size.height, current.height - y );
            for( std::uint32_t x = 0U; x < current.width; )
            {
                const auto width = std::min( tile_size.width, current.width - x );
                if( tile_changed( previous, current, x, y, width, height ) )
                {
                    result.dirty_tiles.push_back( geometry::Rectangle{
                        .x      = static_cast<std::int32_t>( x ),
                        .y      = static_cast<std::int32_t>( y ),
                        .width  = width,
                        .height = height,
                    } );
                }
                x += width;
            }
            y += height;
        }
        if( !result.dirty_tiles.empty() )
        {
            result.kind = TileDiffKind::DirtyTiles;
        }
        return result;
    }

}    // namespace grab::kernel
