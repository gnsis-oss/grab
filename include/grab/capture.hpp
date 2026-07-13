#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/ids.hpp"
#include "grab/image.hpp"
#include "grab/space.hpp"

#include <cstdint>
#include <string>

namespace grab
{

    struct Frame
    {
            FrameId           id{};
            Image             image{};
            CoordinateSpaceId space{};
            CaptureGeneration generation{};
            std::int64_t      captured_at_ns{};
            SpaceRect         content_rect{};
            double            scale{ 1.0 };
    };

    struct MatchEvidence
    {
            double        confidence{};
            std::string   strategy;
            SpaceRect     region{};
            double        target_offset_x{};
            double        target_offset_y{};
            FrameId       source_frame{};
            std::uint32_t transform_generation{};
            std::int64_t  timestamp_ns{};
    };

}    // namespace grab
