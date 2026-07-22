#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include <compare>
#include <cstdint>

namespace grab
{

    struct CoordinateSpaceId
    {
            std::uint32_t value{};
            friend auto
            operator<=>( const CoordinateSpaceId&,
                         const CoordinateSpaceId& ) = default;
    };

    enum class TransformTrust : std::uint8_t
    {
        Exact,
        Calibrated,
        Heuristic,
        Untrusted,
    };

    struct SpacePoint
    {
            double            x{};
            double            y{};
            CoordinateSpaceId space{};
    };

    struct SpaceRect
    {
            double            x{};
            double            y{};
            double            w{};
            double            h{};
            CoordinateSpaceId space{};
    };

    struct Affine
    {
            double xx{ 1.0 };
            double xy{ 0.0 };
            double tx{ 0.0 };
            double yx{ 0.0 };
            double yy{ 1.0 };
            double ty{ 0.0 };
    };

    struct TransformRecord
    {
            CoordinateSpaceId source{};
            CoordinateSpaceId destination{};
            Affine            map{};
            std::uint64_t     mapping_id{};
            std::uint32_t     generation{};
            TransformTrust    trust{ TransformTrust::Untrusted };
    };

}    // namespace grab
