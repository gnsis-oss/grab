#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/ids.hpp"
#include "grab/space.hpp"

#include <compare>
#include <cstdint>

namespace grab
{

    struct SurfaceId
    {
            std::uint64_t value{};
            friend auto
            operator<=>( const SurfaceId&,
                         const SurfaceId& ) = default;
    };

    struct SurfaceRecord
    {
            SurfaceId         id{};
            DisplayGeneration generation{};
            CoordinateSpaceId space{};
            SpaceRect         bounds{};
    };

}    // namespace grab
