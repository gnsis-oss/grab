#pragma once

#include "grab/image.hpp"
#include "grab/result.hpp"

#include <cstdint>
#include <optional>

namespace grab::image
{

    struct Rect
    {
            std::int32_t  x      = 0;
            std::int32_t  y      = 0;
            std::uint32_t width  = 0U;
            std::uint32_t height = 0U;
    };

    struct CompareOptions
    {
            std::uint8_t per_channel_tolerance = 0U;
    };

    struct DiffResult
    {
            double              match_ratio = 1.0;
            std::uint64_t       diff_pixels = 0U;
            std::optional<Rect> bounding_box;
    };

    [[nodiscard]]
    grab::Result<DiffResult>
    compare( const grab::Image&    a,
             const grab::Image&    b,
             const CompareOptions& opts = {} );

    [[nodiscard]]
    grab::Result<grab::Image>
    diff_image( const grab::Image& a,
                const grab::Image& b );

}    // namespace grab::image
