#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/geometry/rectangle.hpp"
#include "grab/geometry/size.hpp"
#include "grab/image.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace grab::kernel
{

    enum class TileDiffKind : std::uint8_t
    {
        NoChange,
        DirtyTiles,
        FullInvalidation,
    };

    struct TileDiffResult
    {
            TileDiffKind                     kind{ TileDiffKind::NoChange };
            std::vector<geometry::Rectangle> dirty_tiles;
            std::string                      reason;
    };

    struct TileDiffer
    {
            [[nodiscard]]
            TileDiffResult
            diff( const Image&   previous,
                  const Image&   current,
                  geometry::Size tile_size ) const;
    };

}    // namespace grab::kernel
