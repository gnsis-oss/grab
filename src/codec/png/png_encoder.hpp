#pragma once

#include "grab/image.hpp"
#include "grab/result.hpp"

#include <cstdint>
#include <vector>

namespace grab::codec
{

    [[nodiscard]]
    grab::Result<std::vector<std::uint8_t>>
    encode_png( const grab::ImageView& image );

}    // namespace grab::codec
